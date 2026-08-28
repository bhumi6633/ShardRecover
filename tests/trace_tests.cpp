#include "shardrecover/trace.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace shardrecover;
struct Failure : std::runtime_error { using std::runtime_error::runtime_error; };
#define CHECK(x) do { if(!(x)) throw Failure(std::string("CHECK failed: ") + #x); } while(false)

struct Temp {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("shardrecover-trace-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp(){std::filesystem::create_directory(path);} ~Temp(){std::error_code e;std::filesystem::remove_all(path,e);}
};
void write(const std::filesystem::path& path, std::initializer_list<unsigned char> values)
{
    std::ofstream out(path,std::ios::binary); for(auto v:values) out.put(static_cast<char>(v));
}

void serialization_and_escaping()
{
    trace::ReconstructionTrace t;
    t.configuration = {2,1,"indexed",4,"beam",8,"mmap","none","consensus"};
    t.fragments = {{0,"quote\" slash\\ tab\t line\n â.bin",5}};
    t.edges = {{0,1,5,3,2,false,{{0,0,255},{1,128,19}}}};
    t.selected_path = {4,1,7};
    t.reconstruction = {"beam",false,3,4,11,1,2};
    t.mismatches = {{4,1,2,19,122}};
    t.repairs = {{9,"consensus",0,255,2,3,0,false},{10,"png_crc",128,122,0,0,17,true}};
    t.graph_stats = {GraphBuildStrategy::indexed,GraphBuildStrategy::indexed,false,12,4,4,2,4};
    std::ostringstream a,b; trace::write_json(t,a); trace::write_json(t,b);
    CHECK(a.str()==b.str());
    CHECK(a.str().find("\"schema_version\":1")!=std::string::npos);
    CHECK(a.str().find("quote\\\" slash\\\\ tab\\t line\\n â.bin")!=std::string::npos);
    CHECK(a.str().find("\"left_byte\":0,\"right_byte\":255")!=std::string::npos);
    CHECK(a.str().find("\"selected_path\":[4,1,7]")!=std::string::npos);
    CHECK(a.str().find("\"repairs\":[")!=std::string::npos);
    CHECK(a.str().find("original_offset")==std::string::npos);
    CHECK(a.str().find("ground_truth")==std::string::npos);
    CHECK(a.str().find("fragment_bytes")==std::string::npos);
}

void real_engine_trace()
{
    Temp temp;
    const auto a=temp.path/"a.bin", b=temp.path/"b.bin";
    write(a,{1,2,3,4,5}); write(b,{3,9,5,6,7});
    std::vector<BinaryFile> files;
    files.push_back(BinaryFile::load(a));
    files.push_back(BinaryFile::load(b));
    GraphBuildStats stats;
    auto graph=FragmentGraph::build(files,GraphBuildConfig{3,1,GraphBuildStrategy::exhaustive,1},&stats);
    auto result=GreedyReconstructor::reconstruct(graph,files);
    auto t=trace::Builder::build(files,graph,result,
        trace::Configuration{3,1,"exhaustive",1,"greedy",0,"buffered","none","none"},stats);
    CHECK(t.version==1); CHECK(t.fragments.size()==2); CHECK(t.edges.size()==1);
    CHECK(!t.edges[0].exact); CHECK(t.edges[0].mismatches==1);
    CHECK(t.edges[0].mismatch_details[0].overlap_offset==1);
    CHECK(t.selected_path.size()==2); CHECK(t.mismatches.size()==1);
    CHECK(t.reconstruction.fragments_total==2);
    std::ostringstream out; trace::write_json(t,out);
    CHECK(out.str().find("\"mismatch_details\":[{")!=std::string::npos);
}

void repair_format_and_write_failure()
{
    trace::ReconstructionTrace t;
    t.reconstruction={"greedy",true,0,0,0,0,0};
    t.format.type="png"; t.format.signature_valid=true; t.format.structurally_valid=true;
    t.format.width=16; t.format.height=8; t.format.valid_crc_count=3; t.format.all_crc_valid=true;
    t.format.chunks={{0,8,"IHDR",13,true}};
    std::ostringstream out; trace::write_json(t,out);
    CHECK(out.str().find("\"width\":16")!=std::string::npos);
    CHECK(out.str().find("\"type\":\"IHDR\"")!=std::string::npos);
    Temp temp; bool failed=false;
    try { trace::write_json_file(t,temp.path/"missing"/"trace.json"); }
    catch(const std::runtime_error&){failed=true;}
    CHECK(failed);
}
}

int main()
{
    const std::vector<std::pair<std::string,void(*)()>> tests={{"serialization and escaping",serialization_and_escaping},{"real engine trace",real_engine_trace},{"repair, format, and write failure",repair_format_and_write_failure}};
    int failed=0; for(const auto&[name,test]:tests){try{test();std::cout<<"PASS: "<<name<<'\n';}catch(const std::exception&e){++failed;std::cerr<<"FAIL: "<<name<<": "<<e.what()<<'\n';}}
    return failed?1:0;
}
