#include "platform/digest.hpp"
#include <iostream>
#include <stdexcept>

void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){
    try{
        check(opt::sha256("")=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","Empty SHA-256 vector failed");
        check(opt::sha256("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","ABC SHA-256 vector failed");
        opt::Sha256 digest;const std::string chunk(1000,'a');for(unsigned i=0;i<1000;++i)digest.update(chunk);
        check(digest.finish()=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0","Incremental SHA-256 vector failed");
        const auto a=opt::CanonicalKey{}.text("a","ab").text("b","c").finish();
        const auto b=opt::CanonicalKey{}.text("a","a").text("b","bc").finish();
        check(a!=b,"Ambiguous field concatenation");
        check(opt::CanonicalKey{}.text("a","1").finish()!=opt::CanonicalKey{}.number("a",1).finish(),"Key field types collided");
        check(opt::CanonicalKey{}.number("a",1).finish()!=opt::CanonicalKey{}.signed_number("a",1).finish(),"Signed and unsigned keys collided");
        check(opt::new_uuid()!=opt::new_uuid(),"Page attempt IDs repeated");
        std::cout<<"OS SHA-256, incremental input and canonical key separation passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
