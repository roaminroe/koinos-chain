#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace koinos::mq {

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
   const uint64_t    delivery_tag;
   const std::string exchange;
   const std::string routing_key;
   const std::string content_type;
   const std::string data;
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

   void disconnect() noexcept;

   error_code publish(
      const std::string& routing_key,
      const std::string& data,
      const std::string& content_type = "application/json",
      const std::string& exchange = exchange::event
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