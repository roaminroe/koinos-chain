#include <koinos/state_db/backends/map/map_backend.hpp>

namespace koinos::state_db::backends::map {

map_backend::map_backend() {}

map_backend::~map_backend() {}

iterator map_backend::begin()
{
   return iterator( std::make_unique< map_iterator >( std::make_unique< map_iterator::iterator_impl >( _map.begin() ), _map ) );
}

iterator map_backend::end()
{
   return iterator( std::make_unique< map_iterator >( std::make_unique< map_iterator::iterator_impl >( _map.end() ), _map ) );
}

void map_backend::put( const key_type& k, const value_type& v )
{
   _map.insert_or_assign( k, v );
}

void map_backend::erase( const key_type& k )
{
   _map.erase( k );
}

void map_backend::clear()
{
   _map.clear();
}

map_backend::size_type map_backend::size()const
{
   return _map.size();
}

iterator map_backend::find( const key_type& k )
{
   return iterator( std::make_unique< map_iterator >( std::make_unique< map_iterator::iterator_impl >( _map.find( k ) ), _map ) );
}

iterator map_backend::lower_bound( const key_type& k )
{
   return iterator( std::make_unique< map_iterator >( std::make_unique< map_iterator::iterator_impl >( _map.lower_bound( k ) ), _map ) );
}

} // koinos::state_db::backends::map
