#pragma once
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
// Scratch prototype; canonical tail remains the existing profile rule.

#include <cstring>
#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
namespace zlink::detail::json_profile::byte_blocks {
inline bool available() { return __builtin_cpu_supports("avx2"); }
inline constexpr auto &alphabet=zlink::detail::json_profile::base64_alphabet;
inline constexpr auto &values=zlink::detail::json_profile::base64_values;
struct tables_t{std::array<std::int8_t,32>low{},high{},decode_offset{},encode_offset{};char slash_adjust=0;};
inline constexpr tables_t tables=[] {
 tables_t t;std::array<unsigned,16>allowed{},groups{};size_t count=0;
 for(unsigned h=0;h<16;++h)for(unsigned l=0;l<16;++l)if(values[h*16+l]!=255)allowed[h]|=1u<<l;
 for(auto mask:allowed)if(mask&&std::find(groups.begin(),groups.begin()+count,mask)==groups.begin()+count)groups[count++]=mask;
 groups[count++]=0;
 for(unsigned h=0;h<16;++h){auto g=std::find(groups.begin(),groups.begin()+count,allowed[h])-groups.begin();t.high[h]=t.high[h+16]=1u<<g;
  for(unsigned l=0;l<16;++l)if(values[h*16+l]!=255){t.decode_offset[h]=t.decode_offset[h+16]=int(values[h*16+l])-int(h*16+l);break;}}
 for(unsigned l=0;l<16;++l){unsigned mask=0;for(unsigned g=0;g<count;++g)if(!(groups[g]&(1u<<l)))mask|=1u<<g;t.low[l]=t.low[l+16]=mask;}
 for(unsigned g=0;g<16;++g){unsigned index=g==0?0:g==1?26:g<12?g+50:g==12?62:g==13?63:0;t.encode_offset[g]=t.encode_offset[g+16]=int(alphabet[index])-int(index);}
 t.slash_adjust=int(t.decode_offset[static_cast<unsigned>('/')>>4])-(int(values['/'])-int('/'));return t;
}();
static_assert([]{for(unsigned c=0;c<256;++c){bool valid=!(tables.low[c&15]&tables.high[c>>4]);if(valid!=(values[c]!=255))return false;if(valid&&c+int(tables.decode_offset[c>>4])-(c=='/'?tables.slash_adjust:0)!=values[c])return false;}return true;}());
__attribute__((target("avx2"))) inline size_t plain_ascii(const char*src,size_t n){
 size_t i=0;
 for(;n-i>=32;i+=32){auto x=_mm256_loadu_si256((const __m256i*)(src+i));auto special=_mm256_or_si256(_mm256_cmpgt_epi8(_mm256_set1_epi8(0x20),x),_mm256_or_si256(_mm256_cmpeq_epi8(x,_mm256_set1_epi8('"')),_mm256_cmpeq_epi8(x,_mm256_set1_epi8('\\'))));auto mask=static_cast<unsigned>(_mm256_movemask_epi8(special));if(mask)return i+__builtin_ctz(mask);}
 return i;
}
__attribute__((target("avx2"))) inline size_t encode_blocks(const unsigned char*src,size_t n,char*dst){
 size_t i=0,o=0;
 const auto arrange=_mm256_setr_epi32(0,1,2,0,3,4,5,0);
 const auto shuffle=_mm256_setr_epi8(1,0,2,1,4,3,5,4,7,6,8,7,10,9,11,10,1,0,2,1,4,3,5,4,7,6,8,7,10,9,11,10);
 for(;n-i>=32;i+=24,o+=32){
  auto x=_mm256_loadu_si256((const __m256i*)(src+i));x=_mm256_permutevar8x32_epi32(x,arrange);x=_mm256_shuffle_epi8(x,shuffle);
  auto a=_mm256_mulhi_epu16(_mm256_and_si256(x,_mm256_set1_epi32(0x0fc0fc00)),_mm256_set1_epi32(0x04000040));
  auto b=_mm256_mullo_epi16(_mm256_and_si256(x,_mm256_set1_epi32(0x003f03f0)),_mm256_set1_epi32(0x01000010));
  auto v=_mm256_or_si256(a,b);
  auto group=_mm256_sub_epi8(_mm256_subs_epu8(v,_mm256_set1_epi8(51)),_mm256_cmpgt_epi8(v,_mm256_set1_epi8(25)));
  auto offsets=_mm256_loadu_si256((const __m256i*)tables.encode_offset.data());
  auto y=_mm256_add_epi8(v,_mm256_shuffle_epi8(offsets,group));
  _mm256_storeu_si256((__m256i*)(dst+o),y);
 }
 return i;
}
__attribute__((target("avx2"))) inline bool decode_blocks(const char*src,size_t n,unsigned char*dst,size_t&read){
 size_t i=0,o=0;
 auto bad=_mm256_setzero_si256();
 for(;n-i>=32;i+=32,o+=24){
  auto x=_mm256_loadu_si256((const __m256i*)(src+i));
  auto lo=_mm256_and_si256(x,_mm256_set1_epi8(15)),hi=_mm256_and_si256(_mm256_srli_epi32(x,4),_mm256_set1_epi8(15));
  auto low_table=_mm256_loadu_si256((const __m256i*)tables.low.data());
  auto high_table=_mm256_loadu_si256((const __m256i*)tables.high.data());
  auto invalid=_mm256_and_si256(_mm256_shuffle_epi8(low_table,lo),_mm256_shuffle_epi8(high_table,hi));
  bad=_mm256_or_si256(bad,invalid);
  auto offsets=_mm256_loadu_si256((const __m256i*)tables.decode_offset.data());
  auto v=_mm256_add_epi8(x,_mm256_shuffle_epi8(offsets,hi));v=_mm256_sub_epi8(v,_mm256_and_si256(_mm256_cmpeq_epi8(x,_mm256_set1_epi8('/')),_mm256_set1_epi8(tables.slash_adjust)));
  auto y=_mm256_madd_epi16(_mm256_maddubs_epi16(v,_mm256_set1_epi32(0x01400140)),_mm256_set1_epi32(0x00011000));
  y=_mm256_shuffle_epi8(y,_mm256_setr_epi8(2,1,0,6,5,4,10,9,8,14,13,12,-128,-128,-128,-128,2,1,0,6,5,4,10,9,8,14,13,12,-128,-128,-128,-128));
  y=_mm256_permutevar8x32_epi32(y,_mm256_setr_epi32(0,1,2,4,5,6,3,7));
  _mm_storeu_si128((__m128i*)(dst+o),_mm256_castsi256_si128(y));_mm_storel_epi64((__m128i*)(dst+o+16),_mm256_extracti128_si256(y,1));
 }
 read=i;return _mm256_testz_si256(bad,bad);
}
}

#else
#include <cstddef>
namespace zlink::detail::json_profile::byte_blocks {
inline bool available(){return false;}
inline std::size_t plain_ascii(const char*,std::size_t){return 0;}
inline std::size_t encode_blocks(const unsigned char*,std::size_t,char*){return 0;}
inline bool decode_blocks(const char*,std::size_t,unsigned char*,std::size_t&read){read=0;return true;}
}
#endif
