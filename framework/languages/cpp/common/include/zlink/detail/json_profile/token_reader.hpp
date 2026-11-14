#pragma once
#include <zlink/detail/json_profile/ascii_prefix.hpp>
// Scratch: one JSON grammar driving the existing typed_sax semantic decisions.
namespace zlink::detail::json_profile {
enum class json_phase : unsigned char { first, value, comma, key };
enum class json_turn { root, done, object_first, object_key, array_first, value, object_comma, array_comma };
class token_reader {
 const char *cur,*last;
 void ws(){while(cur!=last&&(*cur==' '||*cur=='\t'||*cur=='\r'||*cur=='\n'))++cur;}
 bool take(char c){ws();if(cur==last||*cur!=c)return false;++cur;return true;}
 bool literal(std::string_view s){if(size_t(last-cur)<s.size()||memcmp(cur,s.data(),s.size()))return false;cur+=s.size();return true;}
 bool text(std::string_view&view,std::string&owned){
  ws();if(cur==last||*cur++!='"')return false;const char*begin=cur;
  if(byte_blocks::available())cur+=byte_blocks::plain_ascii(cur,last-cur);
  // Reuses the envelope reader's bounded word predicate; no read across the input end.
  while(last-cur>=8){
   uint64_t word;memcpy(&word,cur,8);
   const auto plain=plain_ascii_prefix(word);
   if(plain!=8){cur+=plain;break;}cur+=8;
  }
  while(cur!=last&&static_cast<unsigned char>(*cur)>=0x20&&static_cast<unsigned char>(*cur)<0x80&&*cur!='"'&&*cur!='\\')++cur;
  if(cur==last)return false;
  if(*cur=='"'){view={begin,size_t(cur-begin)};++cur;return true;}
  if(static_cast<unsigned char>(*cur)<0x20)return false;
  // Escaped/UTF-8 strings use the existing nlohmann lexical decoder once, for this token only.
  const char*scan=cur;bool escaped=false;
  while(scan!=last){char c=*scan++;if(escaped){escaped=false;continue;}if(c=='\\'){escaped=true;continue;}if(c=='"')break;}
  if(scan==last&&(last==begin||last[-1]!='"'))return false;
  auto parsed=nlohmann::json::parse(begin-1,scan,nullptr,false);
  if(parsed.is_discarded()||!parsed.is_string())return false;
  owned=std::move(parsed.get_ref<std::string&>());view=owned;cur=scan;return true;
 }
 template<class Sax> bool value(Sax& sax){
  ws();if(cur==last)return false;
  switch(*cur){
   case '{':++cur;return sax.start_object(size_t(-1));
   case '[':++cur;return sax.start_array(size_t(-1));
   case '"':{
    // Canonical Base64's alphabet is a strict subset of unescaped JSON ASCII.
    // The existing byte decoder owns that validation once; escaped tokens still
    // use the same JSON string decoder before that byte decoder.
    if(sax.expects_bytes()){
     const char*begin=cur+1;const auto*quote=static_cast<const char*>(memchr(begin,'"',last-begin));
     if(quote&&!memchr(begin,'\\',quote-begin)){cur=quote+1;return sax.borrowed_string({begin,size_t(quote-begin)});}
    }
    std::string_view s;std::string owned;return text(s,owned)&&sax.borrowed_string(s);
   }
   case 'n':return literal("null")&&sax.null();
   case 't':return literal("true")&&sax.boolean(true);
   case 'f':return literal("false")&&sax.boolean(false);
   default:{
    // Existing number lexical owner handles sign, leading zero, range and finite conversion.
    auto begin=cur;while(cur!=last&&*cur!='\0'&&*cur!=','&&*cur!=']'&&*cur!='}'&&*cur!=' '&&*cur!='\t'&&*cur!='\r'&&*cur!='\n')++cur;
    if(cur==begin||(*begin!='-'&&(*begin<'0'||*begin>'9')))return false;
    return nlohmann::json::sax_parse(begin,cur,&sax);
   }
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
   case json_turn::object_key:{std::string_view name;std::string owned;if(!text(name,owned)||!sax.borrowed_key(name)||!take(':'))return false;break;}
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
