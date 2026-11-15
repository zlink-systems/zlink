#pragma once
#include <zlink/detail/json_profile/ascii_prefix.hpp>
#include <charconv>
#include <clocale>
#include <cstdlib>
// One JSON grammar drives the existing typed_sax semantic decisions.
namespace zlink::detail::json_profile {
enum class json_phase : unsigned char { first, value, comma, key };
enum class json_turn { root, done, object_first, object_key, array_first, value, object_comma, array_comma };
class token_reader {
 const char *cur,*last;
 void ws(){while(cur!=last&&(*cur==' '||*cur=='\t'||*cur=='\r'||*cur=='\n'))++cur;}
 bool take(char c){ws();if(cur==last||*cur!=c)return false;++cur;return true;}
 bool literal(std::string_view s){if(size_t(last-cur)<s.size()||memcmp(cur,s.data(),s.size()))return false;cur+=s.size();return true;}
 template <class Sax> bool number (Sax &sax)
 {
  const auto begin = cur;
  const bool negative = *cur == '-';
  if (negative) ++cur;
  const auto digit = [&] { return cur != last && *cur >= '0' && *cur <= '9'; };
  if (!digit ()) return false;
  if (*cur == '0') ++cur;
  else while (digit ()) ++cur;
  bool floating = false;
  if (cur != last && *cur == '.') {
   floating = true;
   ++cur;
   if (!digit ()) return false;
   do { ++cur; } while (digit ());
  }
  if (cur != last && (*cur == 'e' || *cur == 'E')) {
   floating = true;
   ++cur;
   if (cur != last && (*cur == '+' || *cur == '-')) ++cur;
   if (!digit ()) return false;
   do { ++cur; } while (digit ());
  }
  // Preserve the SAX number kind, including signed integer -0 and integer
  // overflow becoming a floating value. Target ranges remain in typed_sax.
  if (!floating) {
   if (negative) {
    std::int64_t value{};
    if (std::from_chars (begin, cur, value).ec == std::errc{})
     return sax.number_integer (value);
   } else {
    std::uint64_t value{};
    if (std::from_chars (begin, cur, value).ec == std::errc{})
     return sax.number_unsigned (value);
   }
  }
  // strtod is the existing floating conversion owner, including rounding and
  // underflow. Only conversion needs a terminated, locale-adjusted token;
  // every number's JSON grammar is decided above, without a second parser.
  std::string token (begin, cur);
  const auto decimal = std::localeconv ()->decimal_point[0];
  if (decimal != '.') {
   const auto point = token.find ('.');
   if (point != std::string::npos) token[point] = decimal;
  }
  const auto value = std::strtod (token.c_str (), nullptr);
  return std::isfinite (value) ? sax.number_float (value) : sax.syntax_error ();
 }
 template <class Accept> bool text(Accept accept){
  ws();if(cur==last||*cur++!='"')return false;const char*begin=cur;
  // Short keys and values use the same bounded word predicate as the writer
  // and envelope reader. Longer ASCII runs continue with the existing blocks.
  const auto prefix=plain_ascii_prefix(cur,std::min(size_t(last-cur),size_t(32)));
  cur+=prefix;
  if(prefix==32){
   if(byte_blocks::available())cur+=byte_blocks::plain_ascii(cur,last-cur);
   cur+=plain_ascii_prefix(cur,last-cur);
  }
  if(cur==last)return false;
  if(*cur=='"'){const std::string_view view{begin,size_t(cur-begin)};++cur;return accept(view);}
  if(static_cast<unsigned char>(*cur)<0x20)return false;
  // Escaped/UTF-8 strings use the existing nlohmann lexical decoder once, for this token only.
  const char*scan=cur;bool escaped=false;
  while(scan!=last){char c=*scan++;if(escaped){escaped=false;continue;}if(c=='\\'){escaped=true;continue;}if(c=='"')break;}
  if(scan==last&&(last==begin||last[-1]!='"'))return false;
  auto parsed=nlohmann::json::parse(begin-1,scan,nullptr,false);
  if(parsed.is_discarded()||!parsed.is_string())return false;
  cur=scan;return accept(parsed.get_ref<const std::string&>());
 }
 template<class Sax> bool value(Sax& sax){
  ws();if(cur==last)return false;
  switch(*cur){
   case '{':++cur;return sax.start_object();
   case '[':++cur;return sax.start_array();
   case '"':{
    // Canonical Base64's alphabet is a strict subset of unescaped JSON ASCII.
    // The existing byte decoder owns that validation once; escaped tokens still
    // use the same JSON string decoder before that byte decoder.
    if(sax.expects_bytes()){
     const char*begin=cur+1;const auto*quote=static_cast<const char*>(memchr(begin,'"',last-begin));
     if(quote&&!memchr(begin,'\\',quote-begin)){cur=quote+1;return sax.borrowed_string({begin,size_t(quote-begin)});}
    }
    return text([&](std::string_view s){return sax.borrowed_string(s);});
   }
   case 'n':return literal("null")&&sax.null();
   case 't':return literal("true")&&sax.boolean(true);
   case 'f':return literal("false")&&sax.boolean(false);
   default:return number(sax);
  }
 }
public:
 token_reader(const char*b,const char*e):cur(b),last(e){}
 // Match the existing nlohmann end_of_input token, including its NUL sentinel.
 template<class Sax>bool parse(Sax&sax){
  for(;;){ws();switch(sax.next_turn()){
   case json_turn::root:case json_turn::value:if(!value(sax))return false;break;
   case json_turn::done:return cur==last||*cur=='\0';
   case json_turn::object_first:
    if(cur!=last&&*cur=='}'){++cur;if(!sax.end_object())return false;break;}
    [[fallthrough]];
   case json_turn::object_key:{if(!text([&](std::string_view name){return sax.borrowed_key(name);})||!take(':'))return false;break;}
   case json_turn::array_first:
    if(cur!=last&&*cur==']'){++cur;if(!sax.end_array())return false;break;}
    if(!value(sax))return false;break;
   case json_turn::object_comma:
    if(cur!=last&&*cur=='}'){++cur;if(!sax.end_object())return false;break;}
    if(!take(','))return false;sax.after_comma();break;
   case json_turn::array_comma:
    if(cur!=last&&*cur==']'){++cur;if(!sax.end_array())return false;break;}
    if(!take(','))return false;sax.after_comma();break;
  }}
 }
};
}
