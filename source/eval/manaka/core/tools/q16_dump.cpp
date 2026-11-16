// Writes every integer the evaluator produces for the positions of the given TSV files to a binary file, so two
// builds (or two file formats) can be compared byte for byte with cmp. Uses only the API both versions share.
#include "q16.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>
using namespace YaneuraOu::Eval::ManakaCore;
namespace {
BoardView parse(const std::string& s) {
	BoardView b{}; std::istringstream in(s); std::string board,side,hand; in>>board>>side>>hand;
	int rank=0,file=0; bool pro=false; const std::string kinds="PLNSGBRK";
	for(char c:board) {
		if(c=='/') { ++rank; file=0; continue; }
		if(c=='+') { pro=true; continue; }
		if(c>='1'&&c<='9') { file+=c-'0'; continue; }
		char upper=char(std::toupper(static_cast<unsigned char>(c))); size_t k=kinds.find(upper);
		if(pro) { k=k<4?k+8:k+7; }
		b.cell[file*9+rank]=uint8_t(make_piece_code(c==upper?0:1,int(k))); ++file; pro=false;
	}
	b.side_to_move=side=="b"?0:1; int count=0;
	for(char c:hand) {
		if(c=='-') continue;
		if(c>='0'&&c<='9') { count=count*10+c-'0'; continue; }
		char upper=char(std::toupper(static_cast<unsigned char>(c))); size_t k=kinds.find(upper);
		b.hand[c==upper?0:1][k]=uint8_t(count?count:1); count=0;
	}
	return b;
}
template<class T> void put(std::ofstream& o,const T& v){o.write(reinterpret_cast<const char*>(&v),sizeof(v));}
void put_acc(std::ofstream& o,const Q16Accumulator& a){put(o,a.p1);put(o,a.p2);put(o,a.p3);put(o,a.pair);put(o,a.tokens);}
void put_result(std::ofstream& o,const Q16Result& r,bool h2){
	put(o,r.pre);put(o,r.value);put(o,r.cp);put(o,r.r);put(o,r.r2);put(o,r.clamps);put(o,r.x);put(o,r.acc1);
	if(h2){put(o,r.hidden);put(o,r.acc2);}
}
}
int main(int argc,char**argv){
	if(argc<4){std::cerr<<"usage: q16_dump network out.bin tsv...\n";return 2;}
	Q16Network net;std::string e;
	if(!net.load(argv[1],&e)){std::cerr<<e<<"\n";return 1;}
	std::ofstream o(argv[2],std::ios::binary|std::ios::trunc);long n=0;
	for(int a=3;a<argc;++a){
		std::ifstream in(argv[a]);std::string line;std::getline(in,line);Q16Accumulator inc;bool first=true;
		while(std::getline(in,line)){
			std::istringstream ls(line);std::string idx,sfen_field;std::getline(ls,idx,'\t');std::getline(ls,sfen_field,'\t');
			BoardView b=parse(sfen_field);
			Q16Accumulator cold;if(!net.rebuild(b,cold,&e)){std::cerr<<e<<"\n";return 1;}
			if(first){inc=cold;first=false;}else if(!net.update(b,inc,&e)){std::cerr<<e<<"\n";return 1;}
			put_acc(o,cold);put_acc(o,inc);
			Q16Result fast,ref;
			if(!net.evaluate(cold,b.side_to_move,fast,true,&e)||!net.evaluate(inc,b.side_to_move,ref,false,&e)){std::cerr<<e<<"\n";return 1;}
			put_result(o,fast,net.has_hidden2());put_result(o,ref,net.has_hidden2());
			++n;
		}
	}
	if(n==0){std::cerr<<"no positions read\n";return 1;}
	std::cout<<"positions "<<n<<"\n";return 0;
}
