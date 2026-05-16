#pragma once

namespace httplib { class SSLServer; }
class Database;

namespace tf::plaid {
class PlaidApiClient;
class PlaidTokenBroker;
}

namespace tf::data {

void register_plaid_link_handlers(
    httplib::SSLServer& server,
    Database& db,
    tf::plaid::PlaidApiClient& plaid_client,
    tf::plaid::PlaidTokenBroker& plaid_broker);

} // namespace tf::data
