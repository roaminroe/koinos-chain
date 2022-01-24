
#include <koinos/system/system_calls.hpp>

int main()
{
   auto args = koinos::system::get_contract_arguments();
   const auto [ caller, privilege ] = koinos::system::get_caller();

   // zero is user mode, non-zero is kernel mode
   if ( ( args.c_str()[0] == 0 ) != ( privilege == koinos::chain::privilege::user_mode ) )
   {
      if ( args.c_str()[0] == 0 )
      {
         koinos::system::print( "expected user mode, was kernel mode" );
      }
      else
      {
         koinos::system::print( "expected kernel mode, was user mode" );
      }

      return 1;
   }

   return 0;
}