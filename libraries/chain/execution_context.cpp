#include <koinos/chain/state.hpp>
#include <koinos/chain/types.hpp>
#include <koinos/chain/execution_context.hpp>

namespace koinos::chain {

namespace constants {
   const std::string system = "";
}

execution_context::execution_context( std::shared_ptr< vm_manager::vm_backend > vm_backend, chain::intent i ) :
   _vm_backend( vm_backend )
{
   set_intent( i );
}

std::shared_ptr< vm_manager::vm_backend > execution_context::get_backend() const
{
   return _vm_backend;
}

void execution_context::set_state_node( abstract_state_node_ptr node, abstract_state_node_ptr parent )
{
   _current_state_node = node;
   if ( parent )
      _parent_state_node = parent;
   else if ( _current_state_node )
      _parent_state_node = node->get_parent();
   else
      _parent_state_node.reset();
}

abstract_state_node_ptr execution_context::get_state_node() const
{
   return _current_state_node;
}

abstract_state_node_ptr execution_context::get_parent_node() const
{
   // This handles the genesis case
   return _parent_state_node ? _parent_state_node : _current_state_node;
}

void execution_context::clear_state_node()
{
   _current_state_node.reset();
   _parent_state_node.reset();
}

void execution_context::set_block( const protocol::block& block )
{
   _block = &block;
}

const protocol::block* execution_context::get_block() const
{
   return _block;
}

void execution_context::clear_block()
{
   _block = nullptr;
}

void execution_context::set_transaction( const protocol::transaction& trx )
{
   _trx = &trx;
}

const protocol::transaction& execution_context::get_transaction() const
{
   KOINOS_ASSERT( _trx != nullptr, unexpected_access, "transaction does not exist" );
   return *_trx;
}

void execution_context::clear_transaction()
{
   _trx = nullptr;
}

const std::string& execution_context::get_contract_call_args() const
{
   KOINOS_ASSERT( _stack.size() > 1, stack_exception, "stack is empty" );
   return _stack[ _stack.size() - 2 ].call_args;
}

std::string execution_context::get_contract_return() const
{
   KOINOS_ASSERT( _stack.size() > 1, stack_exception, "stack is empty" );
   return _stack[ _stack.size() - 2 ].call_return;
}

uint32_t execution_context::get_contract_entry_point() const
{
   KOINOS_ASSERT( _stack.size() > 1, stack_exception, "stack is empty" );
   return _stack[ _stack.size() - 2 ].entry_point;
}

void execution_context::set_contract_return( const std::string& ret )
{
   KOINOS_ASSERT( _stack.size() > 1, stack_exception, "stack is empty" );
   _stack[ _stack.size() - 2 ].call_return = ret;
}

void execution_context::set_key_authority( const crypto::public_key& key )
{
   _key_auth = key;
}

void execution_context::clear_authority()
{
   _key_auth.reset();
}

void execution_context::push_frame( stack_frame&& frame )
{
   KOINOS_ASSERT( _stack.size() < execution_context::stack_limit, stack_overflow, "apply context stack overflow" );
   _stack.emplace_back( std::move(frame) );
}

stack_frame execution_context::pop_frame()
{
   KOINOS_ASSERT( _stack.size(), stack_exception, "stack is empty" );
   auto frame = _stack[ _stack.size() - 1 ];
   _stack.pop_back();
   return frame;
}

const std::string& execution_context::get_caller() const
{
   if ( _stack.size() > 1 )
      return _stack[ _stack.size() - 2 ].contract_id;

   return constants::system;
}

privilege execution_context::get_caller_privilege() const
{
   if ( _stack.size() > 1 )
      return _stack[ _stack.size() - 2 ].call_privilege;

   return privilege::kernel_mode;
}

uint32_t execution_context::get_caller_entry_point() const
{
   if ( _stack.size() > 1 )
      return _stack[ _stack.size() - 2 ].entry_point;

   return 0;
}

void execution_context::set_privilege( privilege p )
{
   KOINOS_ASSERT( _stack.size() , stack_exception, "stack empty" );
   _stack[ _stack.size() - 1 ].call_privilege = p;
}

privilege execution_context::get_privilege() const
{
   KOINOS_ASSERT( _stack.size() , stack_exception, "stack empty" );
   return _stack[ _stack.size() - 1 ].call_privilege;
}

const std::string& execution_context::get_contract_id() const
{
   for ( int32_t i = _stack.size() - 1; i >= 0; --i )
   {
      if ( _stack[ i ].contract_id.size() )
         return _stack[ i ].contract_id;
   }

   return constants::system;
}

bool execution_context::read_only() const
{
   return _intent == intent::read_only;
}

resource_meter& execution_context::resource_meter()
{
   return _resource_meter;
}

chronicler& execution_context::chronicler()
{
   return _chronicler;
}

std::shared_ptr< session > execution_context::make_session( uint64_t rc )
{
   auto session = std::make_shared< chain::session >( rc );
   resource_meter().set_session( session );
   chronicler().set_session( session );
   return session;
}

chain::receipt& execution_context::receipt()
{
   return _receipt;
}

void execution_context::set_intent( chain::intent i )
{
   _intent = i;
}

chain::intent execution_context::intent() const
{
   return _intent;
}

#pragma message( "Optimize this behavior in to the per block cache" )
uint64_t execution_context::get_compute_bandwidth( const std::string& thunk_name )
{
   static std::map< std::string, uint64_t > local_cache;

   uint64_t compute = 0;
   bool found = false;

   if ( local_cache.find( thunk_name ) == local_cache.end() )
   {
      auto obj = _current_state_node->get_object( state::space::metadata(), state::key::compute_bandwidth_registry );
      KOINOS_ASSERT( obj, unexpected_state, "compute bandwidth registry does not exist" );
      auto compute_registry = util::converter::to< compute_bandwidth_registry >( *obj );

      for ( const auto& entry : compute_registry.entries() )
      {
         if ( entry.name() == thunk_name )
         {
            found = true;
            compute = entry.compute();
            local_cache[ thunk_name ] = compute;
            break;
         }
      }
   }
   else
   {
      found = true;
      compute = local_cache.at( thunk_name );
   }

   KOINOS_ASSERT( found, unexpected_state, "unable to find compute bandwidth for ${t}", ("t", thunk_name) );

   return compute;
}

} // koinos::chain
