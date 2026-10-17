#include "q16.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>
using namespace YaneuraOu::Eval::ManakaCore;
namespace {
void check(bool v,const std::string& e) { if(!v) throw std::runtime_error(e); }
BoardView parse(const std::string& s) {
	BoardView b{}; std::istringstream in(s); std::string board,side,hand; in>>board>>side>>hand;
	int rank=0,file=0; bool pro=false;
	const std::string kinds="PLNSGBRK";
	for(char c:board) {
		if(c=='/') { check(file==9,"SFEN rank"); ++rank; file=0; continue; }
		if(c=='+') { pro=true; continue; }
		if(c>='1'&&c<='9') { file+=c-'0'; continue; }
		char upper=char(std::toupper(static_cast<unsigned char>(c))); size_t k=kinds.find(upper);
		check(k!=std::string::npos&&rank<9&&file<9,"SFEN piece");
		if(pro) { check(k<4||k==5||k==6,"promotion"); k=k<4?k+8:k+7; }
		b.cell[file*9+rank]=uint8_t(make_piece_code(c==upper?0:1,int(k))); ++file; pro=false;
	}
	check(rank==8&&file==9&&!pro,"SFEN board"); b.side_to_move=side=="b"?0:1;
	int count=0;
	for(char c:hand) {
		if(c=='-') continue;
		if(c>='0'&&c<='9') { count=count*10+c-'0'; check(count<=18,"hand overflow"); continue; }
		char upper=char(std::toupper(static_cast<unsigned char>(c))); size_t k=kinds.find(upper);
		check(k<7,"SFEN hand"); b.hand[c==upper?0:1][k]=uint8_t(count?count:1); count=0;
	}
	return b;
}
std::vector<std::string> split(const std::string& line) {
	std::vector<std::string> v; std::istringstream in(line); std::string s;
	while(std::getline(in,s,'\t')) v.push_back(s);
	return v;
}
bool equal(const Q16Accumulator& a,const Q16Accumulator& b) {
	return a.p1==b.p1&&a.p2==b.p2&&a.p3==b.p3&&a.pair==b.pair&&a.tokens==b.tokens;
}
template<class T> int64_t checksum(const T& v) { int64_t s=0; for(size_t i=0;i<v.size();++i)s+=int64_t(i+1)*v[i];return s; }
}
int main(int argc,char**argv) {
	try {
		check(argc>=2,"usage: q16_driver network [reference.tsv ...] [--load-only]");
		std::cout<<std::setprecision(17);
		Q16Network net; std::string error; check(net.load(argv[1],&error),error);
		std::cout<<"loaded "<<argv[1]<<" checksum_verified="<<net.checksum_verified()<<"\n";
		if(argc==3&&std::string(argv[2])=="--load-only") return 0;
		BoardView invalid{}; Q16Accumulator unchanged;check(!net.rebuild(invalid,unchanged,&error),"accepted zero tokens");
		invalid.hand[0][0]=19;check(!net.rebuild(invalid,unchanged,&error),"accepted hand overflow");
		Q16Result invalid_result;check(!net.evaluate(unchanged,0,invalid_result,true,&error),"accepted unbuilt state");
		check(!net.load(argv[1],&error),"accepted reload");
		// Extreme products exceed i32 sums, but each madd pair is safe.
		std::array<int16_t,800> a,b; a.fill(32767);b.fill(32767);
		check(q16_dot(a.data(),b.data(),800,true)==int64_t(800)*32767*32767,"SIMD extreme positive");
		b.fill(-32767); check(q16_dot(a.data(),b.data(),800,true)==-int64_t(800)*32767*32767,"SIMD extreme negative");
		for(int arg=2;arg<argc;++arg) {
			std::ifstream in(argv[arg]); check(bool(in),"open reference");
			std::string ipath=argv[arg]; ipath.insert(ipath.size()-4,".ints"); std::ifstream ints(ipath);check(bool(ints),"open ints");
			std::string vpath=argv[arg];vpath.replace(vpath.size()-4,4,".vectors.txt");std::ifstream vf(vpath);check(bool(vf),"open vectors");
			std::map<int,std::map<std::string,std::vector<int64_t>>> vectors;
			std::string vl;
			while(std::getline(vf,vl)) {
				if(vl.empty()||vl[0]=='#')continue;
				std::istringstream vs(vl);int index;std::string name;vs>>index>>name;int64_t value;
				while(vs>>value)vectors[index][name].push_back(value);
			}
			std::string line,iline;std::getline(in,line);std::getline(ints,iline);
			std::vector<BoardView> boards; Q16Accumulator incremental; bool first=true;
			double maxpre=0,maxvalue=0,maxr=0,maxr2=0; int cpwrong=0,count=0,dyn=0,dyn2=0;
			std::string worst;
			while(std::getline(in,line)) {
				auto fields=split(line); check(fields.size()>=9,"TSV fields"); BoardView board=parse(fields[1]);
				Q16Accumulator cold; check(net.rebuild(board,cold,&error),error);
				if(first) { incremental=cold;first=false; } else check(net.update(board,incremental,&error),error);
				check(equal(cold,incremental),"update/rebuild mismatch");
				Q16Result scalar,simd;check(net.evaluate(cold,board.side_to_move,scalar,false,&error),error);
				check(net.evaluate(incremental,board.side_to_move,simd,true,&error),error);
				check(scalar.x==simd.x&&scalar.hidden==simd.hidden&&scalar.acc1==simd.acc1&&scalar.acc2==simd.acc2&&scalar.pre==simd.pre,"scalar/SIMD mismatch");
				double prediff=std::abs(double(scalar.pre)-std::stod(fields[4]));
				if(prediff>maxpre) {
					maxpre=prediff;std::ostringstream os;os<<std::setprecision(17)<<"max_pre_position index="<<count<<" cpp_pre="<<scalar.pre<<" numpy_pre="<<fields[4]<<" difference="<<prediff<<" sfen="<<fields[1];worst=os.str();
				}
				maxvalue=std::max(maxvalue,std::abs(double(scalar.value)-std::stod(fields[3])));
				check(scalar.clamps==0,"unexpected quantization clamp");dyn+=scalar.r<1;dyn2+=scalar.r2<1;
				maxr=std::max(maxr,std::abs(scalar.r-std::stod(fields[7])));
				maxr2=std::max(maxr2,std::abs(scalar.r2-std::stod(fields[8])));
				if(scalar.cp!=std::stoi(fields[2])) {
					++cpwrong; check(std::abs(scalar.cp-std::stoi(fields[2]))==1,"cp error >1");
					double boundary=double(std::max(std::abs(scalar.cp),std::abs(std::stoi(fields[2]))))*(scalar.pre<0?-1:1);
					std::cout<<"cp_boundary index="<<count<<" cpp="<<scalar.cp<<" numpy="<<fields[2]<<" cpp_pre="<<scalar.pre<<" numpy_pre="<<fields[4]<<" pre_difference="<<double(scalar.pre)-std::stod(fields[4])<<" boundary_pre="<<boundary/1512.173<<" sfen="<<fields[1]<<"\n";
				}
				for(const auto& entry:vectors[count]) {
					const std::string& name=entry.first;const auto& vv=entry.second;
					for(size_t j=0;j<vv.size();++j) {
						int frame=board.side_to_move;if(name.find("opp")!=std::string::npos)frame=1-frame;
						int64_t v=0;
						if(name=="x")v=scalar.x.at(j);
						else if(name=="acc1")v=scalar.acc1.at(j);
						else if(name=="acc2")v=scalar.acc2.at(j);
						else if(name=="h1i")v=scalar.hidden.at(j);
						else if(name.find("P1_")==0)v=cold.p1[frame].at(j);
						else if(name.find("P2_")==0)v=cold.p2[frame].at(j);
						else if(name.find("P3_")==0)v=cold.p3[frame].at(j);
						else if(name.find("pair_")==0)v=cold.pair[frame].at(j);
						else if(name.find("E2_")==0) {auto p=cold.p1[frame].at(j);v=p*p-cold.p2[frame].at(j);}
						else if(name.find("E3_")==0) {auto p=cold.p1[frame].at(j);v=p*p*p-3*p*cold.p2[frame].at(j)+2*cold.p3[frame].at(j);}
						else throw std::runtime_error("unknown vector "+name);
						check(v==vv[j],"full vector mismatch "+name+" row="+std::to_string(count));
					}
				}
				check(std::getline(ints,iline).good(),"ints line"); auto f=split(iline);
				int stm=board.side_to_move,opp=1-stm;int64_t p=cold.p1[stm][0];
				std::vector<int64_t> values={cold.p1[stm][0],cold.p1[stm][1],cold.p2[stm][0],cold.p3[stm][0],p*p-cold.p2[stm][0],p*p*p-3*p*cold.p2[stm][0]+2*cold.p3[stm][0],cold.pair[stm][0],cold.pair[stm][1],cold.pair[opp][0],checksum(scalar.x)};
				int64_t xm=0,am=0;for(auto x:scalar.x)xm=std::max(xm,std::abs(int64_t(x)));for(auto x:scalar.acc1)am=std::max(am,std::abs(x));
				values.insert(values.end(),{xm,scalar.acc1[0],scalar.acc1[1],scalar.acc1[511],checksum(scalar.acc1),am});
				if(net.has_hidden2()) {
					int64_t hm=0,am2=0;for(auto x:scalar.hidden)hm=std::max(hm,int64_t(x));for(auto x:scalar.acc2)am2=std::max(am2,std::abs(x));
					values.insert(values.end(),{checksum(scalar.hidden),hm,scalar.acc2[0],scalar.acc2[255],checksum(scalar.acc2),am2});
				}
				check(f.size()==values.size()+1,"ints columns");
				for(size_t j=0;j<values.size();++j)check(values[j]==std::stoll(f[j+1]),"integer reference mismatch row="+std::to_string(count)+" col="+std::to_string(j+1));
				// Null move and undo leave the absolute-frame state unchanged.
				BoardView null=board;null.side_to_move^=1;check(net.update(null,incremental,&error),error);check(equal(cold,incremental),"null mismatch");
				if(!boards.empty()) {check(net.update(boards.back(),incremental,&error),error);Q16Accumulator undo;check(net.rebuild(boards.back(),undo,&error),error);check(equal(undo,incremental),"undo mismatch");check(net.update(board,incremental,&error),error);}
				boards.push_back(board);++count;
			}
			std::cout<<argv[arg]<<" positions="<<count<<" max_pre="<<maxpre<<" max_value="<<maxvalue<<" cp_mismatch="<<cpwrong<<" max_r="<<maxr<<" max_r2="<<maxr2<<" dynamic_features="<<dyn<<" dynamic_hidden="<<dyn2<<"\n";
			std::cout<<worst<<"\n";
			check(maxpre<5e-6&&maxvalue<5e-6,"value tolerance exceeded");
			if(std::getenv("Q16_PROFILE")&&!boards.empty()) {
				std::vector<int16_t> w1(512*800);std::ifstream weights(argv[1],std::ios::binary);
				weights.seekg(net.has_hidden2()?88473756:88473728);
				weights.read(reinterpret_cast<char*>(w1.data()),w1.size()*2);check(bool(weights),"profile W1 read");
				BoardView moved=boards[0];int from=-1,to=-1;
				for(int i=0;i<81;++i) {if(moved.cell[i]&&from<0)from=i;if(!moved.cell[i]&&to<0)to=i;}
				check(from>=0&&to>=0,"profile board");moved.cell[to]=moved.cell[from];moved.cell[from]=0;moved.side_to_move^=1;
				for(int mode=0;mode<5;++mode) {
					Q16Accumulator acc;check(net.rebuild(boards[0],acc,&error),error);Q16Result result;
					check(net.evaluate(acc,boards[0].side_to_move,result,true,&error),error);
					int64_t guard=0;auto start=std::chrono::steady_clock::now();constexpr int iterations=10000;
					for(int i=0;i<iterations;++i) {
						const BoardView& board=mode==0?boards[size_t(i)%boards.size()]:(i%2?moved:boards[0]);
						if(mode==0) {check(net.rebuild(board,acc,&error),error);guard+=acc.p1[0][0];}
						else if(mode==1) {check(net.update(board,acc,&error),error);guard+=acc.p1[0][0];}
						else if(mode==2) {check(net.evaluate(acc,board.side_to_move,result,true,&error),error);guard+=result.cp;}
						else for(int row=0;row<512;++row)guard+=q16_dot(result.x.data(),w1.data()+row*800,800,mode==3);
					}
					double sec=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
					const char* names[]={"cold_rebuild","one_token_update","evaluate_avx2","W1_avx2","W1_scalar"};
					std::cout<<"profile="<<names[mode]<<" iterations="<<iterations<<" us="<<sec*1e6/iterations<<" guard="<<guard<<"\n";
				}
			}
			for(int mode=0;mode<(std::getenv("Q16_SKIP_BENCH")?0:8);++mode) {
				int64_t guard=0; Q16Accumulator acc;bool initial=true;
				std::vector<BoardView> adjacent;
				if(mode>=4&&!boards.empty()) {
					BoardView moved=boards[0];int from=-1,to=-1;
					for(int i=0;i<81;++i) {if(moved.cell[i]&&from<0)from=i;if(!moved.cell[i]&&to<0)to=i;}
					check(from>=0&&to>=0,"adjacent fixture");moved.cell[to]=moved.cell[from];moved.cell[from]=0;moved.side_to_move^=1;
					for(int i=0;i<10000;++i)adjacent.push_back(i%2?moved:boards[0]);
				}
				const auto& bench=mode>=4?adjacent:boards;
				auto start=std::chrono::steady_clock::now();
				for(const auto& board:bench) {
					if(mode%4<2||initial)check(net.rebuild(board,acc,&error),error);else check(net.update(board,acc,&error),error);
					initial=false;Q16Result r;check(net.evaluate(acc,board.side_to_move,r,mode%2,&error),error);guard+=r.cp;
				}
				double sec=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
				std::cout<<"mode="<<mode<<" us/position="<<sec*1e6/bench.size()<<" guard="<<guard<<"\n";
			}
		}
		return 0;
	} catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
