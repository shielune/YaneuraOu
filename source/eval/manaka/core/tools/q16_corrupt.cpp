#include "q16.h"
#include "q16_mkc.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace YaneuraOu::Eval::ManakaCore;
namespace {
void require(bool b,const std::string& s) {if(!b)throw std::runtime_error(s);}
void patch(const std::string& p,size_t offset,uint32_t v,size_t n) {
	std::fstream f(p,std::ios::binary|std::ios::in|std::ios::out);f.seekp(offset);f.write(reinterpret_cast<const char*>(&v),n);require(bool(f),"patch failed");
}
uint32_t read(const std::string& p,size_t offset,size_t n) {
	uint32_t v=0;std::ifstream f(p,std::ios::binary);f.seekg(offset);f.read(reinterpret_cast<char*>(&v),n);require(bool(f),"read failed");return v;
}
// Flips the bits of mask in one byte and returns the original byte so the caller can restore it.
uint8_t flip(const std::string& p,size_t offset,uint8_t mask) {
	const uint8_t old=uint8_t(read(p,offset,1));patch(p,offset,uint8_t(old^mask),1);return old;
}
// Loads and returns whether the network was accepted; message gets the error text.
bool accepted(const std::string& p,bool require_checksum,std::string* message,bool* verified=nullptr) {
	Q16Network net;std::string e;const bool ok=net.load(p,&e,require_checksum);
	if(message)*message=e;
	if(verified)*verified=ok&&net.checksum_verified();
	return ok;
}
struct Rng { uint64_t s; uint64_t next(){s=s*6364136223846793005ull+1442695040888963407ull;return s>>17;} };
}
int main(int argc,char**argv) {
	try {
		require(argc==4,"usage: q16_corrupt base.bin h2256.bin scratch.bin");int tests=0,legacy_checks=0,checksum_checks=0;
		// CRC-32C reference vector, hardware against software, over every length and alignment of one buffer.
		{
			const char* v="123456789";
			require(q16_crc32c(v,9,true)==0xE3069283u&&q16_crc32c(v,9,false)==0xE3069283u,"crc32c check value");
			std::vector<uint8_t> buf(300);Rng r{7};for(auto& b:buf)b=uint8_t(r.next());
			for(size_t shift=0;shift<9;++shift)for(size_t len=0;len+shift<=buf.size();++len)
				require(q16_crc32c(buf.data()+shift,len,true)==q16_crc32c(buf.data()+shift,len,false),"crc32c hw/sw mismatch");
			++tests;++checksum_checks;
			std::cout<<"crc32c_hardware="<<q16_crc32c_has_hardware()<<" vector_ok=1\n";
		}
		for(int model=1;model<=2;++model) {
			std::string p=argv[3];std::filesystem::copy_file(argv[model],p,std::filesystem::copy_options::overwrite_existing);
			bool h2=model==2;size_t shift=h2?28:0;
			std::vector<std::pair<size_t,uint32_t>> mutations={{0,0},{4,0},{8,0xffffffff},{12,0},{176+shift,0},{600244+shift,0},{88472120+shift,0},{88473724+shift,0},
				{180+shift,0x8000},{600248+shift,0x8000},{88473728+shift,0x8000},
				{88472124+shift,0x7fc00000},{88472124+shift,0x7f800000},
				{89292928+shift,0},{89292928+shift,0xbf800000},{89292928+shift,0x7fc00000},{89294976+shift,0x7fc00000}};
			if(h2)mutations.insert(mutations.end(),{{89297052,0},{89297056,0},{89297056,0xbf800000},{89297056,0x40400000},{89299104,0x8000},{89561248,0},{89562272,0x7fc00000},{89563296,0},{89563300,0x7f800000},{89564324,0x7fc00000}});
			else mutations.insert(mutations.end(),{{89297024,0},{89297028,0x7f800000},{89299076,0x7fc00000}});
			for(auto [offset,value]:mutations) {
				size_t n=value==0x8000?2:4;auto old=read(p,offset,n);patch(p,offset,value,n);
				{Q16Network net;std::string e;require(!net.load(p,&e),"accepted corrupt offset "+std::to_string(offset));}
				patch(p,offset,old,n);++tests;++legacy_checks;
			}
			// Zero blocks must stay zero; maximum valid embedding exercises the i64 Newton bound.
			BoardView board{};for(int i=0;i<40;++i)board.cell[i]=1;
			for(int extreme=0;extreme<2;++extreme) {
				std::fstream f(p,std::ios::binary|std::ios::in|std::ios::out);
				std::vector<int16_t> fill(2344*128,extreme?32767:0);
				f.seekp(180+shift);f.write(reinterpret_cast<const char*>(fill.data()),fill.size()*2);
				std::vector<int16_t> chunk(65536,extreme?32767:0);f.seekp(600248+shift);
				for(size_t n=2745996u*16;n;) {size_t k=std::min(n,chunk.size());f.write(reinterpret_cast<const char*>(chunk.data()),k*2);n-=k;}
				if(extreme) {
					std::vector<float> m(400,1.0f);m[0]=0.5f;m[1]=-0.5f;
					f.seekp(88472124+shift);f.write(reinterpret_cast<const char*>(m.data()),m.size()*4);
				}
				f.close();Q16Network net;std::string e;require(net.load(p,&e),e);
				Q16Accumulator acc;require(net.rebuild(board,acc,&e),e);Q16Result scalar,simd;
				require(net.evaluate(acc,0,scalar,false,&e),e);require(net.evaluate(acc,0,simd,true,&e),e);
				require(scalar.x==simd.x&&scalar.acc1==simd.acc1&&scalar.acc2==simd.acc2&&scalar.pre==simd.pre,"extreme SIMD mismatch");
				if(!extreme)for(auto x:scalar.x)require(x==0,"zero block nonzero");
				else {
					require(acc.p3[0][0]==int64_t(40)*32767*32767*32767,"extreme cube mismatch");
					require(scalar.r==1&&scalar.x[0]==1&&scalar.x[1]==-1,"half-away rounding mismatch");
				}
				++tests;++legacy_checks;
			}
			auto length=std::filesystem::file_size(p);std::filesystem::resize_file(p,length-1);
			{Q16Network net;std::string e;require(!net.load(p,&e),"accepted truncation");++tests;++legacy_checks;}
			std::filesystem::resize_file(p,length+1);
			{Q16Network net;std::string e;require(!net.load(p,&e),"accepted trailing data");++tests;++legacy_checks;}

			// ---- legacy ModelType=SYMq16: accepted without a checksum, with a warning the caller can read ----
			const std::string legacy=argv[model],c=std::string(argv[3])+".c";
			std::string message;bool verified=true;
			require(accepted(legacy,false,&message,&verified)&&!verified,"legacy file must load unverified");
			require(!accepted(legacy,true,&message)&&message.find("checksum required")!=std::string::npos,"require_checksum must refuse SYMq16: "+message);
			tests+=2;++legacy_checks;++checksum_checks;

			// ---- ModelType=SYMq16c ----
			std::string e;require(q16mkc::make_checksummed(legacy,c,&e),e);
			require(accepted(c,false,&message,&verified)&&verified,"SYMq16c must load and report verified: "+message);
			require(accepted(c,true,&message,&verified)&&verified,"SYMq16c must satisfy require_checksum: "+message);
			tests+=2;checksum_checks+=2;
			const size_t cs=shift+1; // every offset after the header string moves by the extra 'c'
			const size_t clen=std::filesystem::file_size(c);
			const size_t trailer=clen-8;
			// checksum_only: the legacy loader's own checks (length, header, section ids, finite/positive scales,
			// -32768) all accept this change, so only the CRC can reject it.
			struct Flip{const char* name;size_t offset;uint8_t mask;bool checksum_only;};
			const size_t value_w=(h2?89563300u:89297028u)+1; // these two are absolute offsets of each file; the header grows by one byte
			std::vector<Flip> flips={
				{"embed first byte",180+cs,0x01,true},{"embed middle byte",300212+cs,0x01,true},
				{"pair first byte",600248+cs,0x01,true},{"pair middle byte",44536184+cs,0x01,true},{"pair last byte",88472119+cs,0x01,true},
				{"m middle (mantissa lsb)",88472924+cs,0x01,true},
				{"w1 middle byte",88883328+cs,0x01,true},{"w1 last byte",89292927+cs,0x01,true},
				{"inv_k1 first (mantissa lsb)",89292928+cs,0x01,true},{"b1 middle (mantissa lsb)",89296000+cs,0x01,true},
				{"value_w middle (mantissa lsb)",value_w+(h2?512u:1024u),0x01,true},
				{"value_b first byte (mantissa lsb)",clen-8-4,0x01,true},
				{"value_b last byte (exponent)",clen-8-1,0x01,true},
				{"trailer crc byte 0",trailer+4,0x01,false},{"trailer crc byte 3",trailer+7,0x80,false},
				{"trailer section id byte 0",trailer,0x01,false},
				{"header string byte",12+10,0x01,false},
			};
			if(h2) {
				flips.push_back({"a1 middle (exponent bit: still a power of two)",89297056+1+1024+2,0x80,false});
				flips.push_back({"w2 middle byte",89299104+1+131072,0x01,true});
				flips.push_back({"inv_k2 first (mantissa lsb)",89561248+1,0x01,true});
				flips.push_back({"b2 first (mantissa lsb)",89562272+1,0x01,true});
			}
			for(const auto& flipcase:flips) {
				const uint8_t old=flip(c,flipcase.offset,flipcase.mask);
				const bool ok=accepted(c,false,&message);
				patch(c,flipcase.offset,old,1);
				require(!ok,std::string("SYMq16c accepted a flipped byte: ")+flipcase.name);
				if(flipcase.checksum_only||std::string(flipcase.name).find("crc")!=std::string::npos)
					require(message.find("checksum mismatch")!=std::string::npos,std::string("expected the checksum to catch: ")+flipcase.name+" ("+message+")");
				std::cout<<"  model="<<model<<" "<<flipcase.name<<" -> rejected: "<<message<<"\n";
				++tests;++checksum_checks;
			}
			require(accepted(c,false,&message,&verified)&&verified,"restored SYMq16c must load again");

			// ---- truncation and extension ----
			{
				const std::string t=std::string(argv[3])+".t";
				const size_t cuts[]={8,5,4,1,0};
				for(size_t cut:cuts) {
					std::filesystem::copy_file(c,t,std::filesystem::copy_options::overwrite_existing);
					if(cut)std::filesystem::resize_file(t,clen-cut);
					else {std::ofstream o(t,std::ios::binary|std::ios::app);o.put('\0');}
					const bool ok=accepted(t,false,&message);
					require(!ok,"SYMq16c accepted a length change of "+std::to_string(cut));
					std::cout<<"  model="<<model<<(cut?" cut "+std::to_string(cut)+" bytes":" one extra byte")<<" -> rejected: "<<message<<"\n";
					++tests;++checksum_checks;
				}
				std::filesystem::copy_file(c,t,std::filesystem::copy_options::overwrite_existing);
				std::filesystem::resize_file(t,12);
				require(!accepted(t,false,&message),"header-only file accepted");++tests;++checksum_checks;
				std::filesystem::remove(t);
			}
			// ---- random single-bit flips over the whole file, and the same flips in the unchecked file ----
			{
				const std::string l=std::string(argv[3])+".l";
				std::filesystem::copy_file(legacy,l,std::filesystem::copy_options::overwrite_existing);
				const uint64_t seed=0x9E3779B97F4A7C15ull+uint64_t(model);
				Rng rng{seed};int done=0,silent=0,body=0;
				for(int i=0;i<150;++i) {
					const size_t offset=size_t(rng.next()%clen);const uint8_t mask=uint8_t(1u<<(rng.next()%8));
					const uint8_t old=flip(c,offset,mask);
					const bool ok=accepted(c,false,&message);patch(c,offset,old,1);
					require(!ok,"SYMq16c accepted a random bit flip at "+std::to_string(offset));++done;
					// The same bit in the SYMq16 file (its offsets are one byte earlier), body bytes only.
					if(offset>=180+cs&&offset<trailer) {
						++body;const uint8_t lold=flip(l,offset-1,mask);
						const bool lok=accepted(l,false,nullptr);patch(l,offset-1,lold,1);silent+=lok;
					}
				}
				std::cout<<"  model="<<model<<" random single-bit flips: SYMq16c rejected "<<done<<"/150; the same flips in the body of the unchecked SYMq16 were accepted "<<silent<<" of "<<body<<"\n";
				tests+=done;checksum_checks+=done;
				std::filesystem::remove(l);
			}
			std::filesystem::remove(c);
		}
		std::filesystem::remove(argv[3]);
		std::cout<<"checks_passed="<<tests<<" legacy_checks="<<legacy_checks<<" checksum_checks="<<checksum_checks<<"\n";
		return 0;
	}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
