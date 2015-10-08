/*
 * Copyright (c) 2011-2015 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin-node.
 *
 * libbitcoin-node is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Affero General Public License with
 * additional permissions to the one published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version. For more information see LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/node/full_node.hpp>

#include <functional>
#include <future>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <bitcoin/blockchain.hpp>
#include <bitcoin/node/config/settings_type.hpp>
#include <bitcoin/node/full_node.hpp>
#include <bitcoin/node/indexer.hpp>
#include <bitcoin/node/logging.hpp>
#include <bitcoin/node/poller.hpp>
#include <bitcoin/node/responder.hpp>
#include <bitcoin/node/session.hpp>

namespace libbitcoin {
namespace node {

using std::placeholders::_1;
using std::placeholders::_2;
using std::placeholders::_3;
using boost::format;
using boost::posix_time::seconds;
using boost::posix_time::minutes;
using namespace boost::filesystem;
using namespace bc::blockchain;
using namespace bc::network;

// Localizable messages.

// Session errors.
#define BN_CONNECTION_START_ERROR \
    "Error starting connection: %1%"
#define BN_SESSION_START_ERROR \
    "Error starting session: %1%"
#define BN_SESSION_STOP_ERROR \
    "Error stopping session: %1%"
#define BN_SESSION_START_HEIGHT_FETCH_ERROR \
    "Error fetching start height: %1%"
#define BN_SESSION_START_HEIGHT_SET_ERROR \
    "Error setting start height: %1%"
#define BN_SESSION_START_HEIGHT \
    "Set start height (%1%)"

// Transaction successes.
#define BN_TX_ACCEPTED \
    "Accepted transaction into memory pool [%1%]"
#define BN_TX_ACCEPTED_WITH_INPUTS \
    "Accepted transaction into memory pool [%1%] with unconfirmed inputs (%2%)"
#define BN_TX_CONFIRMED \
    "Confirmed transaction into blockchain [%1%]"

// Transaction failures.
#define BN_TX_ACCEPT_FAILURE \
    "Failure accepting transaction in memory pool [%1%] %2%"
#define BN_TX_CONFIRM_FAILURE \
    "Failure confirming transaction into blockchain [%1%] %2%"
#define BN_TX_DEINDEX_FAILURE \
    "Failure deindexing transaction [%1%] %2%"
#define BN_TX_INDEX_FAILURE \
    "Failure indexing transaction [%1%] %2%"
#define BN_TX_RECEIVE_FAILURE \
    "Failure receiving transaction [%1%] %2%"

const settings_type full_node::defaults
{
    // [node]
    bc::node::settings
    {
        NODE_THREADS,
        NODE_TRANSACTION_POOL_CAPACITY,
        NODE_PEERS,
        NODE_BLACKLISTS
    },

    // [blockchain]
    bc::chain::settings
    {
        BLOCKCHAIN_THREADS,
        BLOCKCHAIN_BLOCK_POOL_CAPACITY,
        BLOCKCHAIN_HISTORY_START_HEIGHT,
        BLOCKCHAIN_TESTNET_RULES_MAINNET,
        BLOCKCHAIN_DATABASE_PATH,
        BLOCKCHAIN_CHECKPOINTS_MAINNET
    },

    // [network]
    bc::network::settings
    {
        NETWORK_THREADS,
        NETWORK_IDENTIFIER_MAINNET,
        NETWORK_INBOUND_PORT_MAINNET,
        NETWORK_INBOUND_CONNECTION_LIMIT,
        NETWORK_OUTBOUND_CONNECTIONS,
        NETWORK_CONNECT_TIMEOUT_SECONDS,
        NETWORK_CHANNEL_HANDSHAKE_SECONDS,
        NETWORK_CHANNEL_REVIVAL_MINUTES,
        NETWORK_CHANNEL_HEARTBEAT_MINUTES,
        NETWORK_CHANNEL_INACTIVITY_MINUTES,
        NETWORK_CHANNEL_EXPIRATION_MINUTES,
        NETWORK_CHANNEL_GERMINATION_SECONDS,
        NETWORK_HOST_POOL_CAPACITY,
        NETWORK_RELAY_TRANSACTIONS,
        NETWORK_HOSTS_FILE,
        NETWORK_DEBUG_FILE,
        NETWORK_ERROR_FILE,
        NETWORK_SELF,
        NETWORK_SEEDS_MAINNET
    }
};

constexpr auto append = std::ofstream::out | std::ofstream::app;

/* TODO: create a configuration class for thread priority. */
full_node::full_node(const settings_type& config)
  : debug_file_(
        config.network.debug_file.string(),
        append),
    error_file_(
        config.network.error_file.string(),
        append),
    network_threads_(
        config.network.threads,
        thread_priority::low),
    host_pool_(
        network_threads_,
        config.network.hosts_file,
        config.network.host_pool_capacity),
    network_(
        network_threads_,
        config.network.identifier,
        config.timeouts),
    protocol_(
        network_threads_,
        host_pool_,
        network_,
        config.network.inbound_port,
        config.network.relay_transactions,
        config.network.outbound_connections,
        config.network.inbound_connection_limit,
        config.network.seeds,
        config.network.self,
        config.timeouts),

    database_threads_(
        config.chain.threads,
        thread_priority::low),
    blockchain_(
        database_threads_,
        config.chain.database_path.string(),
        config.chain.history_start_height,
        config.chain.block_pool_capacity,
        config.chain.use_testnet_rules,
        config.chain.checkpoints),
   
    memory_threads_(
        config.node.threads,
        thread_priority::low),
    tx_pool_(
        memory_threads_,
        blockchain_,
        config.node.transaction_pool_capacity),
    tx_indexer_(
        memory_threads_),
    poller_(
        memory_threads_,
        blockchain_),
    responder_(
        blockchain_,
        tx_pool_),
    session_(
        /* TODO: use node threads here? */
        network_threads_,
        protocol_,
        blockchain_,
        poller_,
        tx_pool_,
        responder_, 
        config.minimum_start_height())
{
}

static std::string format_blacklist(const config::authority& authority)
{
    auto formatted = authority.to_string();
    if (authority.port() == 0)
        formatted += ":*";

    return formatted;
}

bool full_node::start(const settings_type& config)
{
    // Set up logging for node background threads.
    initialize_logging(debug_file_, error_file_, bc::cout, bc::cerr,
        config.log_to_skip());

    // Start the blockchain.
    if (!blockchain_.start())
        return false;

    // Register the transaction pool against reorg notifications.
    if (!tx_pool_.start())
        return false;

    std::promise<code> height_promise;
    blockchain_.fetch_last_height(
        std::bind(&full_node::set_height,
            this, _1, _2, std::ref(height_promise)));

    // Wait for set height completion.
    if (height_promise.get_future().get())
        return false;

    // Add banned connections before starting the session.
    for (const auto& authority: config.node.blacklists)
    {
        log_info(LOG_NODE)
            << "Blacklisted peer [" << format_blacklist(authority) << "]";
        protocol_.blacklist(authority);
    }

    // Add configured connections before starting the session.
    for (const auto& endpoint: config.node.peers)
    {
        log_info(LOG_NODE)
            << "Connecting peer [" << endpoint << "]";
        protocol_.maintain_connection(endpoint.host(), endpoint.port(),
            config.network.relay_transactions);
    }

    std::promise<code> session_promise;
    session_.start(
        std::bind(&full_node::handle_start,
            this, _1, std::ref(session_promise)));

    // Wait for start completion.
    const auto started = !session_promise.get_future().get();
    return started;
}

bool full_node::stop()
{
    // Use promise to block on main thread until stop completes.
    std::promise<code> promise;

    // Stop the session.
    session_.stop(
        std::bind(&full_node::handle_stop,
            this, _1, std::ref(promise)));

    // Wait for stop completion.
    auto success = !promise.get_future().get();

    // Try and close blockchain database even if session stop failed.
    // Blockchain stop is currently non-blocking, so the result is misleading.
    // No need to stop tx_pool, it will get a shutdown notification from this.
    if (!blockchain_.stop())
        success = false;

    // Signal the threadpools to stop.
    network_threads_.shutdown();
    database_threads_.shutdown();
    memory_threads_.shutdown();

    // Wait for threads to finish.
    network_threads_.join();
    database_threads_.join();
    memory_threads_.join();

    return success;
}

block_chain& full_node::blockchain()
{
    return blockchain_;
}

transaction_pool& full_node::transaction_pool()
{
    return tx_pool_;
}

node::indexer& full_node::transaction_indexer()
{
    return tx_indexer_;
}

network::protocol& full_node::protocol()
{
    return protocol_;
}

threadpool& full_node::pool()
{
    return memory_threads_;
}

void full_node::handle_start(const code& ec,
    std::promise<code>& promise)
{
    if (ec)
    {
        log_error(LOG_NODE)
            << format(BN_SESSION_START_ERROR) % ec.message();
        promise.set_value(ec);
        return;
    }

    // Subscribe to new connections.
    protocol_.subscribe_channel(
        std::bind(&full_node::new_channel,
            this, _1, _2));

    promise.set_value(ec);
}

void full_node::set_height(const code& ec, uint64_t height,
    std::promise<code>& promise)
{
    if (ec)
    {
        log_error(LOG_SESSION)
            << format(BN_SESSION_START_HEIGHT_FETCH_ERROR) % ec.message();
        promise.set_value(ec);
        return;
    }

    const auto handle_set_height = [height, &promise]
        (const code& ec)
    {
        if (ec)
            log_error(LOG_SESSION)
                << format(BN_SESSION_START_HEIGHT_SET_ERROR) % ec.message();
        else
            log_info(LOG_SESSION)
                << format(BN_SESSION_START_HEIGHT) % height;

        promise.set_value(ec);
    };

    // HACK: not setting start height yet.
    handle_set_height(ec);
    ////handshake_.set_start_height(height, handle_set_height);
}

void full_node::handle_stop(const code& ec, std::promise<code>& promise)
{
    if (ec)
        log_error(LOG_NODE)
            << format(BN_SESSION_STOP_ERROR) % ec.message();

    promise.set_value(ec);
}

void full_node::new_channel(const code& ec, channel::ptr node)
{
    // This is the sentinel code for protocol stopping (and node is nullptr).
    if (ec == error::service_stopped)
        return;

    if (ec)
    {
        log_info(LOG_NODE)
            << format(BN_CONNECTION_START_ERROR) % ec.message();
        return;
    }

    // Subscribe to transaction messages from this node.
    node->subscribe<message::transaction>(
        std::bind(&full_node::recieve_tx,
            this, _1, _2, node));

    // Stay subscribed to new connections.
    protocol_.subscribe_channel(
        std::bind(&full_node::new_channel,
            this, _1, _2));
}

void full_node::recieve_tx(const code& ec,
    const chain::transaction& tx, channel::ptr node)
{
    if (ec == error::channel_stopped)
        return;

    if (ec)
    {
        log_debug(LOG_NODE)
            << format(BN_TX_RECEIVE_FAILURE) % node->address() % ec.message();
        return;
    }

    // Called when the transaction becomes confirmed in a block.
    const auto handle_confirm = [this, tx](const code& ec)
    {
        const auto hash = encode_hash(tx.hash());

        if (ec)
            log_warning(LOG_NODE)
            << format(BN_TX_CONFIRM_FAILURE) % hash % ec.message();
        else
            log_debug(LOG_NODE)
                << format(BN_TX_CONFIRMED) % hash;

        const auto handle_deindex = [hash](const code& ec)
        {
            if (ec)
                log_error(LOG_NODE)
                    << format(BN_TX_DEINDEX_FAILURE) % hash % ec.message();
        };

        tx_indexer_.deindex(tx, handle_deindex);
    };

    // Validate and store the tx in the transaction mempool.
    tx_pool_.store(tx, handle_confirm,
        std::bind(&full_node::new_unconfirm_valid_tx,
            this, _1, _2, tx));

    // Resubscribe to receive transaction messages from this node.
    node->subscribe<message::transaction>(
        std::bind(&full_node::recieve_tx,
            this, _1, _2, node));
}

static std::string format_unconfirmed_inputs(
    const chain::index_list& unconfirmed)
{
    if (unconfirmed.empty())
        return "";

    std::vector<std::string> inputs;
    for (const auto input: unconfirmed)
        inputs.push_back(boost::lexical_cast<std::string>(input));

    return bc::join(inputs, ",");
}

void full_node::new_unconfirm_valid_tx(const code& ec,
    const chain::index_list& unconfirmed, const chain::transaction& tx)
{
    const auto hash = encode_hash(tx.hash());

    const auto handle_index = [hash](const code& ec)
    {
        if (ec)
            log_error(LOG_NODE)
                << format(BN_TX_INDEX_FAILURE) % hash % ec.message();
    };

    if (ec)
        log_debug(LOG_NODE)
            << format(BN_TX_ACCEPT_FAILURE) % hash % ec.message();
    else if (unconfirmed.empty())
        log_debug(LOG_NODE)
            << format(BN_TX_ACCEPTED) % hash;
    else
        log_debug(LOG_NODE)
            << format(BN_TX_ACCEPTED_WITH_INPUTS) % hash %
                format_unconfirmed_inputs(unconfirmed);
        
    if (!ec)
        tx_indexer_.index(tx, handle_index);
}

// HACK: this is for access to broadcast_new_blocks to facilitate server
// inheritance of full_node. The organization should be refactored.
void full_node::broadcast_new_blocks(const code& ec, uint64_t fork_point,
    const block_chain::list& new_blocks,
    const block_chain::list& replaced_blocks)
{
    session_.broadcast_new_blocks(ec, fork_point, new_blocks, replaced_blocks);
}

} // namspace node
} //namespace libbitcoin
