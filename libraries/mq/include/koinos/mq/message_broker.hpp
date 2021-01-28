#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace koinos::mq {

// TODO:  Move these Koinos-specific names elsewhere
namespace exchange {
   constexpr const char* event = "koinos_event";
   constexpr const char* rpc = "koinos_rpc";
} // exchange

namespace routing_key {
   constexpr const char* block_accept = "koinos.block.accept";
   constexpr const char* transaction_accept = "koinos.transaction.accept";
} // routing_key

enum class error_code : int64_t
{
   success,
   failure,
   time_out
};

struct message
{
   uint64_t    delivery_tag;
   std::string exchange;
   std::string routing_key;
   std::string content_type;
   std::optional< std::string > reply_to;
   std::optional< std::string > correlation_id;
   std::string data;
};

namespace detail { struct message_broker_impl; }

class message_broker final
{
private:
   std::unique_ptr< detail::message_broker_impl > _message_broker_impl;

public:
   message_broker();
   ~message_broker();

   error_code connect(
      const std::string& host,
      uint16_t port,
      const std::string& vhost = "/",
      const std::string& user = "guest",
      const std::string& pass = "guest"
   ) noexcept;

   error_code connect_to_url(
      const std::string& url
   ) noexcept;

   void disconnect() noexcept;

   bool is_connected() noexcept;

   error_code publish(
      const message& msg
   ) noexcept;

   std::pair< error_code, std::optional< message > > consume() noexcept;

   error_code queue_declare( const std::string& queue ) noexcept;
   error_code queue_bind(
      const std::string& queue,
      const std::string& exchange,
      const std::string& binding_key
   ) noexcept;
};

} // koinos::mq
