#include "shardrecover/trace.hpp"

#include <fstream>
#include <iomanip>
#include <stdexcept>

namespace shardrecover::trace {
namespace {
void string(std::ostream& out, std::string_view value)
{
    out << '"';
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20) {
                const auto flags = out.flags();
                const auto fill = out.fill();
                out << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
                    << static_cast<unsigned int>(ch);
                out.flags(flags); out.fill(fill);
            } else out << static_cast<char>(ch);
        }
    }
    out << '"';
}
const char* boolean(bool value) { return value ? "true" : "false"; }
void mismatch(std::ostream& out, const Mismatch& value)
{
    out << "{\"overlap_offset\":" << value.overlap_offset << ",\"left_byte\":"
        << static_cast<unsigned int>(value.left_byte) << ",\"right_byte\":"
        << static_cast<unsigned int>(value.right_byte) << '}';
}
}  // namespace

void write_json(const ReconstructionTrace& t, std::ostream& out)
{
    out << "{\n  \"schema_version\":" << t.version << ",\n  \"engine\":{\"name\":";
    string(out, t.engine_name); out << ",\"version\":"; string(out, t.engine_version);
    out << "},\n  \"configuration\":{\"min_overlap\":" << t.configuration.minimum_overlap
        << ",\"max_mismatches\":" << t.configuration.maximum_mismatches
        << ",\"graph_build\":"; string(out, t.configuration.graph_build);
    out << ",\"threads\":" << t.configuration.threads << ",\"strategy\":";
    string(out, t.configuration.reconstruction); out << ",\"beam_width\":" << t.configuration.beam_width
        << ",\"io\":"; string(out, t.configuration.io); out << ",\"format\":";
    string(out, t.configuration.format); out << ",\"repair\":"; string(out, t.configuration.repair);
    out << "},\n  \"fragments\":[";
    for (std::size_t i=0;i<t.fragments.size();++i) { if(i) out<<','; const auto& f=t.fragments[i];
        out << "{\"id\":"<<f.id<<",\"name\":"; string(out,f.display_name); out<<",\"size\":"<<f.size<<'}'; }
    out << "],\n  \"edges\":[";
    for (std::size_t i=0;i<t.edges.size();++i) { if(i) out<<','; const auto& e=t.edges[i];
        out<<"{\"from\":"<<e.from<<",\"to\":"<<e.to<<",\"overlap\":"<<e.overlap
           <<",\"matches\":"<<e.matches<<",\"mismatches\":"<<e.mismatches<<",\"exact\":"<<boolean(e.exact)<<",\"mismatch_details\":[";
        for(std::size_t j=0;j<e.mismatch_details.size();++j){if(j)out<<',';mismatch(out,e.mismatch_details[j]);} out<<"]}"; }
    out << "],\n  \"selected_path\":[";
    for(std::size_t i=0;i<t.selected_path.size();++i){if(i)out<<',';out<<t.selected_path[i];}
    const auto& r=t.reconstruction;
    out << "],\n  \"reconstruction\":{\"strategy\":"; string(out,r.strategy);
    out<<",\"complete\":"<<boolean(r.complete)<<",\"fragments_used\":"<<r.fragments_used
       <<",\"fragments_total\":"<<r.fragments_total<<",\"output_bytes\":"<<r.output_bytes
       <<",\"approximate_joins\":"<<r.approximate_joins<<",\"overlap_mismatches\":"<<r.overlap_mismatches<<"},\n  \"mismatches\":[";
    for(std::size_t i=0;i<t.mismatches.size();++i){if(i)out<<',';const auto&m=t.mismatches[i];out<<"{\"from\":"<<m.from<<",\"to\":"<<m.to<<",\"overlap_offset\":"<<m.overlap_offset<<",\"left_byte\":"<<unsigned(m.left_byte)<<",\"right_byte\":"<<unsigned(m.right_byte)<<'}';}
    out << "],\n  \"repairs\":[";
    for(std::size_t i=0;i<t.repairs.size();++i){if(i)out<<',';const auto&p=t.repairs[i];out<<"{\"position\":"<<p.position<<",\"stage\":";string(out,p.stage);out<<",\"before\":"<<unsigned(p.before)<<",\"after\":"<<unsigned(p.after)<<",\"support\":"<<p.support<<",\"observations\":"<<p.observations<<",\"candidates_tested\":"<<p.candidates_tested<<",\"crc_consistent\":"<<boolean(p.crc_consistent)<<'}';}
    out << "],\n  \"unresolved_ambiguities\":"<<t.unresolved_ambiguities<<",\n  \"format\":{\"type\":";string(out,t.format.type);
    out<<",\"signature_valid\":"<<boolean(t.format.signature_valid)<<",\"structurally_valid\":"<<boolean(t.format.structurally_valid)<<",\"width\":";
    if(t.format.width)out<<*t.format.width;else out<<"null";out<<",\"height\":";if(t.format.height)out<<*t.format.height;else out<<"null";
    out<<",\"valid_crc_count\":"<<t.format.valid_crc_count<<",\"invalid_crc_count\":"<<t.format.invalid_crc_count<<",\"all_crc_valid\":"<<boolean(t.format.all_crc_valid)<<",\"chunks\":[";
    for(std::size_t i=0;i<t.format.chunks.size();++i){if(i)out<<',';const auto&c=t.format.chunks[i];out<<"{\"index\":"<<c.index<<",\"offset\":"<<c.offset<<",\"type\":";string(out,c.type);out<<",\"length\":"<<c.length<<",\"crc_valid\":"<<boolean(c.crc_valid)<<'}';}out<<"]},\n  \"graph_stats\":{\"strategy\":";string(out,t.graph_stats.effective_strategy==GraphBuildStrategy::indexed?"indexed":"exhaustive");
    out<<",\"theoretical_pairs\":"<<t.graph_stats.theoretical_pairs<<",\"candidate_pairs\":"<<t.graph_stats.candidate_pairs<<",\"full_overlap_checks\":"<<t.graph_stats.full_overlap_checks<<",\"edges_created\":"<<t.graph_stats.edges_created<<",\"threads_used\":"<<t.graph_stats.threads_used<<"}\n}\n";
    if (!out) throw std::runtime_error("Failed to serialize reconstruction trace");
}

void write_json_file(const ReconstructionTrace& trace, const std::filesystem::path& path)
{
    std::ofstream output(path, std::ios::trunc);
    if (!output) throw std::runtime_error("Failed to create trace output: '" + path.string() + "'");
    write_json(trace, output); output.close();
    if (!output) throw std::runtime_error("Failed to write trace output: '" + path.string() + "'");
}
}  // namespace shardrecover::trace
