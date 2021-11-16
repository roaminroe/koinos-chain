#include <boost/test/unit_test.hpp>

#include <koinos/bigint.hpp>
#include <koinos/crypto/multihash.hpp>
#include <koinos/log.hpp>
#include <koinos/exception.hpp>
#include <koinos/state_db/backends/map/map_backend.hpp>
#include <koinos/state_db/backends/rocksdb/rocksdb_backend.hpp>
#include <koinos/state_db/detail/merge_iterator.hpp>
#include <koinos/state_db/detail/objects.hpp>
#include <koinos/state_db/detail/state_delta.hpp>
#include <koinos/state_db/state_db.hpp>
#include <koinos/util/conversion.hpp>

#include <boost/container/deque.hpp>
#include <boost/interprocess/streams/vectorstream.hpp>

#include <iostream>
#include <filesystem>

using namespace koinos;
using namespace koinos::state_db;
using state_db::detail::merge_index;
using state_db::detail::state_delta;

using vectorstream = boost::interprocess::basic_vectorstream< std::vector< char > >;
using namespace std::string_literals;

struct test_block
{
   std::string       previous;
   uint64_t          height = 0;
   uint64_t          nonce = 0;

   crypto::multihash get_id() const;
};

struct book
{
   typedef uint64_t id_type;

   template<typename Constructor, typename Allocator>
   book( Constructor&& c, Allocator&& a )
   {
      c(*this);
   }

   book() = default;

   id_type id;
   int a = 0;
   int b = 1;

   int sum()const { return a + b; }
};

struct by_id;
struct by_a;
struct by_b;
struct by_sum;

typedef mira::multi_index_adapter<
   book,
   koinos::state_db::detail::state_object_serializer,
   mira::multi_index::indexed_by<
      mira::multi_index::ordered_unique< mira::multi_index::tag< by_id >, mira::multi_index::member< book, book::id_type, &book::id > >,
      mira::multi_index::ordered_unique< mira::multi_index::tag< by_a >,  mira::multi_index::member< book, int,           &book::a  > >,
      mira::multi_index::ordered_unique< mira::multi_index::tag< by_b >,
         mira::multi_index::composite_key< book,
            mira::multi_index::member< book, int, &book::b >,
            mira::multi_index::member< book, int, &book::a >
         >,
         mira::multi_index::composite_key_compare< std::less< int >, std::less< int > >
      >,
      mira::multi_index::ordered_unique< mira::multi_index::tag< by_sum >, mira::multi_index::const_mem_fun< book, int, &book::sum > >
  >
> book_index;

crypto::multihash test_block::get_id() const
{
   return crypto::hash( crypto::multicodec::sha2_256, util::converter::to< crypto::multihash >( previous ), height, nonce );
}

namespace koinos {

template<>
void to_binary< book >( std::ostream& o, const book& b )
{
   to_binary( o, b.id );
   to_binary( o, b.a );
   to_binary( o, b.b );
}

template<>
void from_binary< book >( std::istream& o, book& b )
{
   from_binary( o, b.id );
   from_binary( o, b.a );
   from_binary( o, b.b );
}

}

struct state_db_fixture
{
   state_db_fixture()
   {
      initialize_logging( "koinos_test", {}, "info" );

      temp = std::filesystem::temp_directory_path() / boost::filesystem::unique_path().string();
      std::filesystem::create_directory( temp );

      db.open( temp );
   }

   ~state_db_fixture()
   {
      boost::log::core::get()->remove_all_sinks();
      db.close();
      std::filesystem::remove_all( temp );
   }

   database db;
   std::filesystem::path temp;
};

BOOST_FIXTURE_TEST_SUITE( state_db_tests, state_db_fixture )

BOOST_AUTO_TEST_CASE( basic_test )
{ try {
   BOOST_TEST_MESSAGE( "Creating book" );
   object_space space = util::converter::as< object_space >( 0 );
   book book_a;
   book_a.id = 1;
   book_a.a = 3;
   book_a.b = 4;
   book get_book;

   crypto::multihash state_id = crypto::hash( crypto::multicodec::sha2_256, 1 );
   auto state_1 = db.create_writable_node( db.get_head()->id(), state_id );
   auto book_a_id = util::converter::as< object_key >( book_a.id );
   auto book_value = util::converter::as< object_value >( book_a );
   BOOST_REQUIRE( state_1->put_object( space, book_a_id, &book_value ) == book_value.size() );

   // Book should not exist on older state node
   BOOST_REQUIRE( db.get_root()->get_object( space, book_a_id ) == nullptr );

   auto ptr = state_1->get_object( space, book_a_id );
   BOOST_REQUIRE( ptr );

   get_book = util::converter::to< book >( *ptr );
   BOOST_REQUIRE_EQUAL( get_book.id, book_a.id );
   BOOST_REQUIRE_EQUAL( get_book.a, book_a.a );
   BOOST_REQUIRE_EQUAL( get_book.b, book_a.b );

   BOOST_TEST_MESSAGE( "Modifying book" );

   book_a.a = 5;
   book_a.b = 6;
   book_value = util::converter::as< object_value >( book_a );
   BOOST_REQUIRE( state_1->put_object( space, book_a_id, &book_value ) == 0 );

   ptr = state_1->get_object( space, book_a_id );
   BOOST_REQUIRE( ptr );

   get_book = util::converter::to< book >( *ptr );
   BOOST_REQUIRE_EQUAL( get_book.id, book_a.id );
   BOOST_REQUIRE_EQUAL( get_book.a, book_a.a );
   BOOST_REQUIRE_EQUAL( get_book.b, book_a.b );

   state_id = crypto::hash( crypto::multicodec::sha2_256, 2 );
   auto state_2 = db.create_writable_node( state_1->id(), state_id );
   BOOST_REQUIRE( !state_2 );

   db.finalize_node( state_1->id() );


   BOOST_REQUIRE_THROW( state_1->put_object( space, book_a_id, &book_value ), node_finalized );

   state_2 = db.create_writable_node( state_1->id(), state_id );
   book_a.a = 7;
   book_a.b = 8;
   book_value = util::converter::as< object_value >( book_a );
   BOOST_REQUIRE( state_2->put_object( space, book_a_id, &book_value ) == 0 );

   ptr = state_2->get_object( space, book_a_id );
   BOOST_REQUIRE( ptr );

   get_book = util::converter::to< book >( *ptr );
   BOOST_REQUIRE_EQUAL( get_book.id, book_a.id );
   BOOST_REQUIRE_EQUAL( get_book.a, book_a.a );
   BOOST_REQUIRE_EQUAL( get_book.b, book_a.b );

   ptr = state_1->get_object( space, book_a_id );
   BOOST_REQUIRE( ptr );

   get_book = util::converter::to< book >( *ptr );
   BOOST_REQUIRE_EQUAL( get_book.id, book_a.id );
   BOOST_REQUIRE_EQUAL( get_book.a, 5 );
   BOOST_REQUIRE_EQUAL( get_book.b, 6 );

   BOOST_TEST_MESSAGE( "Erasing book" );
   BOOST_REQUIRE( state_2->put_object( space, book_a_id, nullptr ) == -1 * book_value.size() );

   BOOST_REQUIRE( !state_2->get_object( space, book_a_id ) );

   db.discard_node( state_2->id() );
   state_2 = db.get_node( state_2->id() );
   BOOST_REQUIRE( !state_2 );

   ptr = state_1->get_object( space, book_a_id );
   BOOST_REQUIRE( ptr );

   get_book = util::converter::to< book >( *ptr );
   BOOST_REQUIRE_EQUAL( get_book.id, book_a.id );
   BOOST_REQUIRE_EQUAL( get_book.a, 5 );
   BOOST_REQUIRE_EQUAL( get_book.b, 6 );

} KOINOS_CATCH_LOG_AND_RETHROW(info) }

BOOST_AUTO_TEST_CASE( fork_tests )
{ try {
   BOOST_TEST_MESSAGE( "Basic fork tests on state_db" );
   crypto::multihash id, prev_id, block_1000_id;
   test_block b;

   prev_id = db.get_root()->id();

   for( uint64_t i = 1; i <= 2000; ++i )
   {
      b.previous = util::converter::as< std::string >( prev_id );
      b.height = i;
      id = b.get_id();

      auto new_block = db.create_writable_node( prev_id, id );
      BOOST_CHECK_EQUAL( b.height, new_block->revision() );
      db.finalize_node( id );

      prev_id = id;

      if( i == 1000 ) block_1000_id = id;
   }

   BOOST_REQUIRE( db.get_root()->id() == crypto::multihash::zero( crypto::multicodec::sha2_256 ) );
   BOOST_REQUIRE( db.get_root()->revision() == 0 );

   BOOST_REQUIRE( db.get_head()->id() == prev_id );
   BOOST_REQUIRE( db.get_head()->revision() == 2000 );

   BOOST_REQUIRE( db.get_node( block_1000_id )->id() == block_1000_id );
   BOOST_REQUIRE( db.get_node( block_1000_id )->revision() == 1000 );

   auto fork_heads = db.get_fork_heads();
   BOOST_REQUIRE_EQUAL( fork_heads.size(), 1 );
   BOOST_REQUIRE( fork_heads[0]->id() == db.get_head()->id() );

   BOOST_TEST_MESSAGE( "Test commit" );
   db.commit_node( block_1000_id );
   BOOST_REQUIRE( db.get_root()->id() == block_1000_id );
   BOOST_REQUIRE( db.get_root()->revision() == 1000 );

   fork_heads = db.get_fork_heads();
   BOOST_REQUIRE_EQUAL( fork_heads.size(), 1 );
   BOOST_REQUIRE( fork_heads[0]->id() == db.get_head()->id() );

   crypto::multihash block_2000_id = id;

   BOOST_TEST_MESSAGE( "Test discard" );
   b.previous = util::converter::as< std::string >( db.get_head()->id() );
   b.height = db.get_head()->revision() + 1;
   id = b.get_id();
   db.create_writable_node( util::converter::to< crypto::multihash >( b.previous ), id );
   auto new_block = db.get_node( id );
   BOOST_REQUIRE( new_block );

   fork_heads = db.get_fork_heads();
   BOOST_REQUIRE_EQUAL( fork_heads.size(), 1 );
   BOOST_REQUIRE( fork_heads[0]->id() == prev_id );

   db.discard_node( id );

   BOOST_REQUIRE( db.get_head()->id() == prev_id );
   BOOST_REQUIRE( db.get_head()->revision() == 2000 );

   fork_heads = db.get_fork_heads();
   BOOST_REQUIRE_EQUAL( fork_heads.size(), 1 );
   BOOST_REQUIRE( fork_heads[0]->id() == prev_id );

   // Shared ptr should still exist, but not be returned with get_node
   BOOST_REQUIRE( new_block );
   BOOST_REQUIRE( !db.get_node( id ) );
   new_block.reset();

   // Cannot discard head
   BOOST_REQUIRE_THROW( db.discard_node( prev_id ), cannot_discard );

   BOOST_TEST_MESSAGE( "Check duplicate node creation" );
   BOOST_REQUIRE( !db.create_writable_node( db.get_head()->parent_id(), db.get_head()->id() ) );

   BOOST_TEST_MESSAGE( "Check failed linking" );
   crypto::multihash zero = crypto::multihash::zero( crypto::multicodec::sha2_256 );
   BOOST_REQUIRE( !db.create_writable_node( zero, id ) );

   crypto::multihash head_id = db.get_head()->id();
   uint64_t head_rev = db.get_head()->revision();

   BOOST_TEST_MESSAGE( "Test minority fork" );
   auto fork_node = db.get_node_at_revision( 1995 );
   prev_id = fork_node->id();
   b.nonce = 1;

   auto old_block_1996_id = db.get_node_at_revision( 1996 )->id();
   auto old_block_1997_id = db.get_node_at_revision( 1997 )->id();

   for ( uint64_t i = 1; i <= 5; ++i )
   {
      b.previous = util::converter::as< std::string >( prev_id );
      b.height = fork_node->revision() + i;
      id = b.get_id();

      auto new_block = db.create_writable_node( prev_id, id );
      BOOST_CHECK_EQUAL( b.height, new_block->revision() );
      db.finalize_node( id );

      BOOST_CHECK( db.get_head()->id() == head_id );
      BOOST_CHECK( db.get_head()->revision() == head_rev );

      prev_id = id;
   }

   fork_heads = db.get_fork_heads();
   BOOST_REQUIRE_EQUAL( fork_heads.size(), 2 );
   BOOST_REQUIRE( ( fork_heads[0]->id() == db.get_head()->id() && fork_heads[1]->id() == id ) ||
                  ( fork_heads[1]->id() == db.get_head()->id() && fork_heads[0]->id() == id ) );
   auto old_head_id = db.get_head()->id();

   b.previous = util::converter::as< std::string >( prev_id );
   b.height = head_rev + 1;
   id = b.get_id();

   // When this node finalizes, it will be the longest path and should become head
   new_block = db.create_writable_node( prev_id, id );
   BOOST_CHECK_EQUAL( b.height, new_block->revision() );

   BOOST_CHECK( db.get_head()->id() == head_id );
   BOOST_CHECK( db.get_head()->revision() == head_rev );

   db.finalize_node( id );

   fork_heads = db.get_fork_heads();
   BOOST_REQUIRE_EQUAL( fork_heads.size(), 2 );
   BOOST_REQUIRE( ( fork_heads[0]->id() == id && fork_heads[1]->id() == old_head_id ) ||
                  ( fork_heads[1]->id() == id && fork_heads[0]->id() == old_head_id ) );

   BOOST_CHECK( db.get_head()->id() == id );
   BOOST_CHECK( db.get_head()->revision() == b.height );

   db.discard_node( old_block_1997_id );
   fork_heads = db.get_fork_heads();
   BOOST_REQUIRE_EQUAL( fork_heads.size(), 2 );
   BOOST_REQUIRE( ( fork_heads[0]->id() == id && fork_heads[1]->id() == old_block_1996_id ) ||
                  ( fork_heads[1]->id() == id && fork_heads[0]->id() == old_block_1996_id ) );

   db.discard_node( old_block_1996_id );
   fork_heads = db.get_fork_heads();
   BOOST_REQUIRE_EQUAL( fork_heads.size(), 1 );
   BOOST_REQUIRE( fork_heads[0]->id() == id );

} KOINOS_CATCH_LOG_AND_RETHROW(info) }

BOOST_AUTO_TEST_CASE( merge_iterator )
{ try {
   /**
    * The merge iterator test was originally written to work with chainbase.
    * The state delta code has since been moved to state db, where the interface
    * has changed. Because this test is intended to test to correctness of the
    * merge iterators only, they will operate directly on state deltas, outside
    * of state_db.
    */
   std::filesystem::path temp = std::filesystem::temp_directory_path() / boost::filesystem::unique_path().string();
   std::filesystem::create_directory( temp );
   std::any cfg = mira::utilities::default_database_configuration();

   using state_delta_type = state_delta< book_index >;
   using state_delta_ptr = std::shared_ptr< state_delta_type >;

   boost::container::deque< state_delta_ptr > delta_deque;
   delta_deque.emplace_back( std::make_shared< state_delta_type >( temp, cfg ) );

   // Book 0: a: 5, b: 10, sum: 15
   // Book 1: a: 1, b: 7, sum: 8
   // Book 2: a: 10, b:3, sum 13
   delta_deque.back()->emplace( [&]( book& b )
   {
      b.a = 5;
      b.b = 10;
   });

   delta_deque.back()->emplace( [&]( book& b )
   {
      b.a = 1;
      b.b = 7;
   });

   delta_deque.back()->emplace( [&]( book& b )
   {
      b.a = 10;
      b.b = 3;
   });

   // Undo State 0 orders:
   // by_a: 1, 0, 2
   // by_b: 2, 1, 0
   // by_sum: 1, 2, 0
   {
      auto by_id_idx = merge_index< book_index, by_id >( delta_deque.back() );
      auto id_itr = by_id_idx.begin();

      BOOST_REQUIRE( id_itr != by_id_idx.end() );
      BOOST_REQUIRE_EQUAL( id_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 5 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 10 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 7 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      ++id_itr;
      BOOST_REQUIRE( id_itr == by_id_idx.end() );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 7 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 5 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 10 );

      const auto id_ptr = by_id_idx.find( 1 );
      BOOST_REQUIRE( id_ptr != nullptr );
      BOOST_REQUIRE_EQUAL( id_ptr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_ptr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_ptr->b, 7 );

      auto by_a_idx = merge_index< book_index, by_a >( delta_deque.back() );
      auto a_itr = by_a_idx.begin();

      BOOST_REQUIRE( a_itr != by_a_idx.end() );
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 7 );
      ++a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 5 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 10 );
      ++a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 3 );
      ++a_itr;
      BOOST_REQUIRE( a_itr == by_a_idx.end() );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 3 );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 5 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 10 );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 7 );

      auto by_b_idx = merge_index< book_index, by_b >( delta_deque.back() );
      auto b_itr = by_b_idx.begin();

      BOOST_REQUIRE( b_itr != by_b_idx.end() );
      BOOST_REQUIRE_EQUAL( b_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 3 );
      ++b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 7 );
      ++b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 5 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 10 );
      ++b_itr;
      BOOST_REQUIRE( b_itr == by_b_idx.end() );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 5 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 10 );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 7 );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 3 );

      auto by_sum_idx = merge_index< book_index, by_sum >( delta_deque.back() );
      auto sum_itr = by_sum_idx.begin();

      BOOST_REQUIRE( sum_itr != by_sum_idx.end() );
      BOOST_REQUIRE_EQUAL( sum_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 7 );
      ++sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
      ++sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 5 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 10 );
      ++sum_itr;
      BOOST_REQUIRE( sum_itr == by_sum_idx.end() );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 5 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 10 );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 7 );
   }

   // Book 0: a: 2, b: 13, sum: 15
   // Book 1: a: 3, b: 5, sum: 8
   // Book 2: a: 10, b: 3, sum: 13 (not changed)
   delta_deque.emplace_back( std::make_shared< state_delta_type >( delta_deque.back(), delta_deque.back()->id() ) );
   const auto book_0 = delta_deque.back()->template find< by_id >( 0 );
   BOOST_REQUIRE( book_0 != nullptr );
   BOOST_REQUIRE_EQUAL( book_0->id, 0 );
   BOOST_REQUIRE_EQUAL( book_0->a, 5 );
   BOOST_REQUIRE_EQUAL( book_0->b, 10 );
   delta_deque.back()->modify( *book_0, [&]( book& b )
   {
      b.a = 2;
      b.b = 13;
   });

   const auto book_1 = delta_deque.back()->template find< by_id >( 1 );
   BOOST_REQUIRE( book_1 != nullptr );
   BOOST_REQUIRE_EQUAL( book_1->id, 1 );
   BOOST_REQUIRE_EQUAL( book_1->a, 1 );
   BOOST_REQUIRE_EQUAL( book_1->b, 7 );
   delta_deque.back()->modify( *book_1, [&]( book& b )
   {
      b.a = 3;
      b.b = 5;
   });

   // Undo State 1 orders:
   // by_a: 0, 1, 2
   // by_b: 2, 1, 0 (not changed)
   // by_sum: 1, 2, 0 (not changed)
   {
      auto by_id_idx = merge_index< book_index, by_id >( delta_deque.back() );
      auto id_itr = by_id_idx.begin();

      BOOST_REQUIRE( id_itr != by_id_idx.end() );
      BOOST_REQUIRE_EQUAL( id_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 13 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 3 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 5 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      ++id_itr;
      BOOST_REQUIRE( id_itr == by_id_idx.end() );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 3 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 5 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 13 );

      const auto id_ptr = by_id_idx.find( 1 );
      BOOST_REQUIRE( id_ptr != nullptr );
      BOOST_REQUIRE_EQUAL( id_ptr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_ptr->a, 3 );
      BOOST_REQUIRE_EQUAL( id_ptr->b, 5 );

      auto by_a_idx = merge_index< book_index, by_a >( delta_deque.back() );
      auto a_itr = by_a_idx.begin();

      BOOST_REQUIRE( a_itr != by_a_idx.end() );
      BOOST_REQUIRE_EQUAL( a_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 13 );
      ++a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 3 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 5 );
      ++a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 3 );
      ++a_itr;
      BOOST_REQUIRE( a_itr == by_a_idx.end() );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 3 );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 3 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 5 );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 13 );

      auto by_b_idx = merge_index< book_index, by_b >( delta_deque.back() );
      auto b_itr = by_b_idx.begin();

      BOOST_REQUIRE( b_itr != by_b_idx.end() );
      BOOST_REQUIRE_EQUAL( b_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 3 );
      ++b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 3 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 5 );
      ++b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 13 );
      ++b_itr;
      BOOST_REQUIRE( b_itr == by_b_idx.end() );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 13 );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 3 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 5 );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 3 );

      auto by_sum_idx = merge_index< book_index, by_sum >( delta_deque.back() );
      auto sum_itr = by_sum_idx.begin();

      BOOST_REQUIRE( sum_itr != by_sum_idx.end() );
      BOOST_REQUIRE_EQUAL( sum_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 3 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 5 );
      ++sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
      ++sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 13 );
      ++sum_itr;
      BOOST_REQUIRE( sum_itr == by_sum_idx.end() );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 13 );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 3 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 5 );
   }

   // Book 0: a: 2, b: 13, sum: 15 (not changed)
   // Book 1: a: 1, b: 20, sum: 21
   // Book 2: a: 10, b: 3, sum: 13 (not changed)
   delta_deque.emplace_back( std::make_shared< state_delta_type >( delta_deque.back(), delta_deque.back()->id() ) );
   delta_deque.back()->modify( *(delta_deque.back()->template find< by_id >( 1 )), [&]( book& b )
   {
      b.a = 1;
      b.b = 20;
   });

   // Undo State 2 orders:
   // by_a: 1, 0, 2
   // by_b: 2, 0, 1
   // by_sum: 2, 0, 1
   {
      auto by_id_idx = merge_index< book_index, by_id >( delta_deque.back() );
      auto id_itr = by_id_idx.begin();

      BOOST_REQUIRE( id_itr != by_id_idx.end() );
      BOOST_REQUIRE_EQUAL( id_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 13 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 20 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      ++id_itr;
      BOOST_REQUIRE( id_itr == by_id_idx.end() );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 20 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 13 );

      const auto id_ptr = by_id_idx.find( 1 );
      BOOST_REQUIRE( id_ptr != nullptr );
      BOOST_REQUIRE_EQUAL( id_ptr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_ptr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_ptr->b, 20 );

      auto by_a_idx = merge_index< book_index, by_a >( delta_deque.back() );
      auto a_itr = by_a_idx.begin();

      BOOST_REQUIRE( a_itr != by_a_idx.end() );
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 20 );
      ++a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 13 );
      ++a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 3 );
      ++a_itr;
      BOOST_REQUIRE( a_itr == by_a_idx.end() );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 3 );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 13 );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 20 );

      auto by_b_idx = merge_index< book_index, by_b >( delta_deque.back() );
      auto b_itr = by_b_idx.begin();

      BOOST_REQUIRE( b_itr != by_b_idx.end() );
      BOOST_REQUIRE_EQUAL( b_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 3 );
      ++b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 13 );
      ++b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 20 );
      ++b_itr;
      BOOST_REQUIRE( b_itr == by_b_idx.end() );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 20 );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 13 );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 3 );

      auto by_sum_idx = merge_index< book_index, by_sum >( delta_deque.back() );
      auto sum_itr = by_sum_idx.begin();

      BOOST_REQUIRE( sum_itr != by_sum_idx.end() );
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
      ++sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 13 );
      ++sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 20 );
      ++sum_itr;
      BOOST_REQUIRE( sum_itr == by_sum_idx.end() );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 20 );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 13 );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
   }

   // Book: 0 (removed)
   // Book 1: a: 1, b: 20, sum: 21 (not changed)
   // Book 2: a: 10, b: 3, sum: 13 (not changed)
   delta_deque.emplace_back( std::make_shared< state_delta_type >( delta_deque.back(), delta_deque.back()->id() ) );
   delta_deque.back()->erase( *(delta_deque.back()->template find< by_id >( 0 )) );

   // Undo State 3 orders:
   // by_a: 1, 2
   // by_b: 2, 1
   // by_sum: 2, 1
   {
      auto by_id_idx = merge_index< book_index, by_id >( delta_deque.back() );
      auto id_itr = by_id_idx.begin();

      BOOST_REQUIRE( id_itr != by_id_idx.end() );
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 20 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      ++id_itr;
      BOOST_REQUIRE( id_itr == by_id_idx.end() );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 20 );

      const auto id_ptr = by_id_idx.find( 0 );
      BOOST_REQUIRE_EQUAL( id_ptr, nullptr );

      auto by_a_idx = merge_index< book_index, by_a >( delta_deque.back() );
      auto a_itr = by_a_idx.begin();

      BOOST_REQUIRE( a_itr != by_a_idx.end() );
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 20 );
      ++a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 3 );
      ++a_itr;
      BOOST_REQUIRE( a_itr == by_a_idx.end() );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 3 );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 20 );

      auto by_b_idx = merge_index< book_index, by_b >( delta_deque.back() );
      auto b_itr = by_b_idx.begin();

      BOOST_REQUIRE( b_itr != by_b_idx.end() );
      BOOST_REQUIRE_EQUAL( b_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 3 );
      ++b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 20 );
      ++b_itr;
      BOOST_REQUIRE( b_itr == by_b_idx.end() );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 20 );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 3 );

      auto by_sum_idx = merge_index< book_index, by_sum >( delta_deque.back() );
      auto sum_itr = by_sum_idx.begin();

      BOOST_REQUIRE( sum_itr != by_sum_idx.end() );
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
      ++sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 20 );
      ++sum_itr;
      BOOST_REQUIRE( sum_itr == by_sum_idx.end() );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 20 );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
   }

   // Book 1: a: 1, b: 20, sum: 21 (not changed)
   // Book 2: a: 10, b: 3, sum: 13 (not changed)
   // Book 3: a: 2, b: 13, sum: 15 (old book 0)
   delta_deque.emplace_back( std::make_shared< state_delta_type >( delta_deque.back(), delta_deque.back()->id() ) );
   delta_deque.back()->emplace( [&]( book& b )
   {
      b.a = 2;
      b.b = 13;
   });

   // Undo State 4 orders:
   // by_a: 1, 3, 2
   // by_b: 2, 3, 1
   // by_sum: 2, 3, 1
   {
      auto by_id_idx = merge_index< book_index, by_id >( delta_deque.back() );
      auto id_itr = by_id_idx.begin();

      BOOST_REQUIRE( id_itr != by_id_idx.end() );
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 20 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 13 );
      ++id_itr;
      BOOST_REQUIRE( id_itr == by_id_idx.end() );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 13 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 20 );

      const auto id_ptr = by_id_idx.find( 3 );
      BOOST_REQUIRE( id_ptr != nullptr );
      BOOST_REQUIRE_EQUAL( id_ptr->id, 3 );
      BOOST_REQUIRE_EQUAL( id_ptr->a, 2 );
      BOOST_REQUIRE_EQUAL( id_ptr->b, 13 );

      auto by_a_idx = merge_index< book_index, by_a >( delta_deque.back() );
      auto a_itr = by_a_idx.begin();

      BOOST_REQUIRE( a_itr != by_a_idx.end() );
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 20 );
      ++a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 13 );
      ++a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 3 );
      ++a_itr;
      BOOST_REQUIRE( a_itr == by_a_idx.end() );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 3 );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 13 );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 20 );

      auto by_b_idx = merge_index< book_index, by_b >( delta_deque.back() );
      auto b_itr = by_b_idx.begin();

      BOOST_REQUIRE( b_itr != by_b_idx.end() );
      BOOST_REQUIRE_EQUAL( b_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 3 );
      ++b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 13 );
      ++b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 20 );
      ++b_itr;
      BOOST_REQUIRE( b_itr == by_b_idx.end() );

      auto by_sum_idx = merge_index< book_index, by_sum >( delta_deque.back() );
      auto sum_itr = by_sum_idx.begin();

      BOOST_REQUIRE( sum_itr != by_sum_idx.end() );
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
      ++sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 13 );
      ++sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 20 );
      ++sum_itr;
      BOOST_REQUIRE( sum_itr == by_sum_idx.end() );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 20 );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 13 );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
   }

   delta_deque.pop_front();
   delta_deque.pop_front();
   delta_deque.front()->commit();
   {
      auto by_id_idx = merge_index< book_index, by_id >( delta_deque.back() );
      auto id_itr = by_id_idx.begin();

      BOOST_REQUIRE( id_itr != by_id_idx.end() );
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 20 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 13 );
      ++id_itr;
      BOOST_REQUIRE( id_itr == by_id_idx.end() );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 13 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 20 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 20 );
      ++id_itr;
      ++id_itr;
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );

      auto by_a_idx = merge_index< book_index, by_a >( delta_deque.back() );
      auto a_itr = by_a_idx.begin();

      BOOST_REQUIRE( a_itr != by_a_idx.end() );
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 20 );
      ++a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 13 );
      ++a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 3 );
      ++a_itr;
      BOOST_REQUIRE( a_itr == by_a_idx.end() );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 3 );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 13 );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 20 );

      auto by_b_idx = merge_index< book_index, by_b >( delta_deque.back() );
      auto b_itr = by_b_idx.begin();

      BOOST_REQUIRE( b_itr != by_b_idx.end() );
      BOOST_REQUIRE_EQUAL( b_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 3 );
      ++b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 13 );
      ++b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 20 );
      ++b_itr;
      BOOST_REQUIRE( b_itr == by_b_idx.end() );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 20 );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 13 );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 3 );

      auto by_sum_idx = merge_index< book_index, by_sum >( delta_deque.back() );
      auto sum_itr = by_sum_idx.begin();

      BOOST_REQUIRE( sum_itr != by_sum_idx.end() );
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
      ++sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 13 );
      ++sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 20 );
      ++sum_itr;
      BOOST_REQUIRE( sum_itr == by_sum_idx.end() );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 20 );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 13 );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
   }

   while( delta_deque.size() > 1 )
   {
      delta_deque.pop_front();
      delta_deque.front()->commit();

      auto by_id_idx = merge_index< book_index, by_id >( delta_deque.back() );
      auto id_itr = by_id_idx.begin();

      BOOST_REQUIRE( id_itr != by_id_idx.end() );
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 20 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 13 );
      ++id_itr;
      BOOST_REQUIRE( id_itr == by_id_idx.end() );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 13 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 20 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 20 );
      ++id_itr;
      ++id_itr;
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );

      auto by_a_idx = merge_index< book_index, by_a >( delta_deque.back() );
      auto a_itr = by_a_idx.begin();

      BOOST_REQUIRE( a_itr != by_a_idx.end() );
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 20 );
      ++a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 13 );
      ++a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 3 );
      ++a_itr;
      BOOST_REQUIRE( a_itr == by_a_idx.end() );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 3 );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 13 );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 20 );
      ++a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 13 );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 20 );
      ++a_itr;
      ++a_itr;
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 13 );

      auto by_b_idx = merge_index< book_index, by_b >( delta_deque.back() );
      auto b_itr = by_b_idx.begin();

      BOOST_REQUIRE( b_itr != by_b_idx.end() );
      BOOST_REQUIRE_EQUAL( b_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 3 );
      ++b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 13 );
      ++b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 20 );
      ++b_itr;
      BOOST_REQUIRE( b_itr == by_b_idx.end() );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 20 );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 13 );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 3 );
      ++b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 13 );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 3 );
      ++b_itr;
      ++b_itr;
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 13 );

      auto by_sum_idx = merge_index< book_index, by_sum >( delta_deque.back() );
      auto sum_itr = by_sum_idx.begin();

      BOOST_REQUIRE( sum_itr != by_sum_idx.end() );
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
      ++sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 13 );
      ++sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 20 );
      ++sum_itr;
      BOOST_REQUIRE( sum_itr == by_sum_idx.end() );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 20 );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 13 );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
      ++sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 13 );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
      ++sum_itr;
      ++sum_itr;
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 13 );
   }
} KOINOS_CATCH_LOG_AND_RETHROW(info) }

BOOST_AUTO_TEST_CASE( reset_test )
{ try {
   BOOST_TEST_MESSAGE( "Creating book" );
   object_space space = util::converter::as< object_space >( 0 );
   book book_a;
   book_a.id = 1;
   book_a.a = 3;
   book_a.b = 4;
   book get_book;

   crypto::multihash state_id = crypto::hash( crypto::multicodec::sha2_256, 1 );
   auto state_1 = db.create_writable_node( db.get_head()->id(), state_id );
   auto book_a_id = util::converter::as< object_key >( book_a.id );
   auto book_value = util::converter::as< object_value >( book_a );

   BOOST_REQUIRE( state_1->put_object( space, book_a_id, &book_value ) == book_value.size() );
   state_1.reset();

   BOOST_TEST_MESSAGE( "Resetting database" );
   db.reset();
   auto head = db.get_head();

   // Book should not exist on reset db
   BOOST_REQUIRE( !head->get_object( space, book_a_id ) );
   BOOST_REQUIRE( head->id() == crypto::multihash::zero( crypto::multicodec::sha2_256 ) );
   BOOST_REQUIRE( head->revision() == 0 );

} KOINOS_CATCH_LOG_AND_RETHROW(info) }

BOOST_AUTO_TEST_CASE( anonymous_node_test )
{ try {
   BOOST_TEST_MESSAGE( "Creating book" );
   object_space space = util::converter::as< object_space >( 0 );
   book book_a;
   book_a.id = 1;
   book_a.a = 3;
   book_a.b = 4;
   book get_book;

   crypto::multihash state_id = crypto::hash( crypto::multicodec::sha2_256, 1 );
   auto state_1 = db.create_writable_node( db.get_head()->id(), state_id );
   auto book_a_id = util::converter::as< object_key >( book_a.id );
   auto book_value = util::converter::as< object_value >( book_a );

   BOOST_REQUIRE( state_1->put_object( space, book_a_id, &book_value ) == book_value.size() );

   auto ptr = state_1->get_object( space, book_a_id );
   BOOST_REQUIRE( ptr );

   get_book = util::converter::to< book >( *ptr );
   BOOST_REQUIRE_EQUAL( get_book.id, book_a.id );
   BOOST_REQUIRE_EQUAL( get_book.a, book_a.a );
   BOOST_REQUIRE_EQUAL( get_book.b, book_a.b );

   {
      BOOST_TEST_MESSAGE( "Creating anonymous state node" );
      auto anon_state = state_1->create_anonymous_node();

      BOOST_REQUIRE( anon_state->id() == state_1->id() );
      BOOST_REQUIRE( anon_state->revision() == state_1->revision() );
      BOOST_REQUIRE( anon_state->parent_id() == state_1->parent_id() );

      BOOST_TEST_MESSAGE( "Modifying book" );

      book_a.a = 5;
      book_a.b = 6;
      book_value = util::converter::as< object_value >( book_a );
      BOOST_REQUIRE( anon_state->put_object( space, book_a_id, &book_value ) == 0 );

      ptr = state_1->get_object( space, book_a_id );
      BOOST_REQUIRE( ptr );

      get_book = util::converter::to< book >( *ptr );
      BOOST_REQUIRE_EQUAL( get_book.id, book_a.id );
      BOOST_REQUIRE_EQUAL( get_book.a, 3 );
      BOOST_REQUIRE_EQUAL( get_book.b, 4 );

      ptr = anon_state->get_object( space, book_a_id );
      BOOST_REQUIRE( ptr );

      get_book = util::converter::to< book >( *ptr );
      BOOST_REQUIRE_EQUAL( get_book.id, book_a.id );
      BOOST_REQUIRE_EQUAL( get_book.a, book_a.a );
      BOOST_REQUIRE_EQUAL( get_book.b, book_a.b );

      BOOST_TEST_MESSAGE( "Deleting anonymous node" );
   }

   {
      BOOST_TEST_MESSAGE( "Creating anonymous state node" );
      auto anon_state = state_1->create_anonymous_node();

      BOOST_TEST_MESSAGE( "Modifying book" );

      book_a.a = 5;
      book_a.b = 6;
      book_value = util::converter::as< object_value >( book_a );
      BOOST_REQUIRE( anon_state->put_object( space, book_a_id, &book_value ) == 0 );

      ptr = state_1->get_object( space, book_a_id );
      BOOST_REQUIRE( ptr );

      get_book = util::converter::to< book >( *ptr );
      BOOST_REQUIRE_EQUAL( get_book.id, book_a.id );
      BOOST_REQUIRE_EQUAL( get_book.a, 3 );
      BOOST_REQUIRE_EQUAL( get_book.b, 4 );

      ptr = anon_state->get_object( space, book_a_id );
      BOOST_REQUIRE( ptr );

      get_book = util::converter::to< book >( *ptr );
      BOOST_REQUIRE_EQUAL( get_book.id, book_a.id );
      BOOST_REQUIRE_EQUAL( get_book.a, book_a.a );
      BOOST_REQUIRE_EQUAL( get_book.b, book_a.b );

      BOOST_TEST_MESSAGE( "Committing anonymous node" );
      anon_state->commit();

      ptr = state_1->get_object( space, book_a_id );
      BOOST_REQUIRE( ptr );

      get_book = util::converter::to< book >( *ptr );
      BOOST_REQUIRE_EQUAL( get_book.id, book_a.id );
      BOOST_REQUIRE_EQUAL( get_book.a, book_a.a );
      BOOST_REQUIRE_EQUAL( get_book.b, book_a.b );
   }

   ptr = state_1->get_object( space, book_a_id );
   BOOST_REQUIRE( ptr );

   get_book = util::converter::to< book >( *ptr );
   BOOST_REQUIRE_EQUAL( get_book.id, book_a.id );
   BOOST_REQUIRE_EQUAL( get_book.a, book_a.a );
   BOOST_REQUIRE_EQUAL( get_book.b, book_a.b );

} KOINOS_CATCH_LOG_AND_RETHROW(info) }

BOOST_AUTO_TEST_CASE( rocksdb_backend_test )
{ try {
   koinos::state_db::backends::rocksdb::rocksdb_backend backend;

   auto itr = backend.begin();
   BOOST_CHECK( itr == backend.end() );

   backend.put( "foo", "bar" );
   itr = backend.begin();
   BOOST_CHECK( itr != backend.end() );
   BOOST_CHECK( *itr == "bar" );

   backend.put( "alice", "bob" );

   itr = backend.begin();
   BOOST_CHECK( itr != backend.end() );
   BOOST_CHECK( *itr == "bob" );

   ++itr;
   BOOST_CHECK( *itr == "bar" );

   ++itr;
   BOOST_CHECK( itr == backend.end() );

   --itr;
   BOOST_CHECK( itr != backend.end() );
   BOOST_CHECK( *itr == "bar" );

   itr = backend.lower_bound( "charlie" );
   BOOST_CHECK( itr != backend.end() );
   BOOST_CHECK( *itr == "bar" );

   itr = backend.lower_bound( "foo" );
   BOOST_CHECK( itr != backend.end() );
   BOOST_CHECK( *itr == "bar" );

   backend.put( "foo", "blob" );
   itr = backend.find( "foo" );
   BOOST_CHECK( itr != backend.end() );
   BOOST_CHECK( *itr == "blob" );

   --itr;
   BOOST_CHECK( itr != backend.end() );
   BOOST_CHECK( *itr == "bob" );

   backend.erase( "foo" );

   itr = backend.begin();
   BOOST_CHECK( itr != backend.end() );
   BOOST_CHECK( *itr == "bob" );

   itr = backend.find( "foo" );
   BOOST_CHECK( itr == backend.end() );

   backend.erase( "foo" );

   backend.erase( "alice" );
   itr = backend.end();
   BOOST_CHECK( itr == backend.end() );

} KOINOS_CATCH_LOG_AND_RETHROW(info) }

BOOST_AUTO_TEST_CASE( map_backend_test )
{ try {
   koinos::state_db::backends::map::map_backend backend;

   auto itr = backend.begin();
   BOOST_CHECK( itr == backend.end() );

   backend.put( "foo", "bar" );
   itr = backend.begin();
   BOOST_CHECK( itr != backend.end() );
   BOOST_CHECK( *itr == "bar" );

   backend.put( "alice", "bob" );

   itr = backend.begin();
   BOOST_CHECK( itr != backend.end() );
   BOOST_CHECK( *itr == "bob" );

   ++itr;
   BOOST_CHECK( *itr == "bar" );

   ++itr;
   BOOST_CHECK( itr == backend.end() );

   --itr;
   BOOST_CHECK( itr != backend.end() );
   BOOST_CHECK( *itr == "bar" );

   itr = backend.lower_bound( "charlie" );
   BOOST_CHECK( itr != backend.end() );
   BOOST_CHECK( *itr == "bar" );

   itr = backend.lower_bound( "foo" );
   BOOST_CHECK( itr != backend.end() );
   BOOST_CHECK( *itr == "bar" );

   backend.put( "foo", "blob" );
   itr = backend.find( "foo" );
   BOOST_CHECK( itr != backend.end() );
   BOOST_CHECK( *itr == "blob" );

   --itr;
   BOOST_CHECK( itr != backend.end() );
   BOOST_CHECK( *itr == "bob" );

   backend.erase( "foo" );

   itr = backend.begin();
   BOOST_CHECK( itr != backend.end() );
   BOOST_CHECK( *itr == "bob" );

   itr = backend.find( "foo" );
   BOOST_CHECK( itr == backend.end() );

   backend.erase( "foo" );

   backend.erase( "alice" );
   itr = backend.end();
   BOOST_CHECK( itr == backend.end() );

} KOINOS_CATCH_LOG_AND_RETHROW(info) }

BOOST_AUTO_TEST_SUITE_END()
