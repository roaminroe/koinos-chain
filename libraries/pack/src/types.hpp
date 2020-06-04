
namespace koinos { namespace protocol {

struct unused_extensions_type { };

struct block_height_type
{
   uint64 height;
};

struct timestamp_type
{
   uint64 timestamp;
};

// KOINOS_SIGNATURE_LENGTH = 65 from fc::ecc::compact_signature
typedef fl_blob<65> signature_type;
// CONTRACT_ID_LENGTH = 20 from ripemd160 = 160 bits
typedef fl_blob<20> contract_id_type;

} }