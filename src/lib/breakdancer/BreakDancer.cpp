#include "BreakDancer.hpp"

#include "BamConfig.hpp"
#include "IBamReader.hpp"
#include "Options.hpp"
#include "SvBuilder.hpp"
#include "Timer.hpp"

#include <boost/array.hpp>
#include <boost/bind.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/math/distributions/chi_squared.hpp>
#include <boost/math/distributions/poisson.hpp>
#include <boost/ref.hpp>

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

#define LZERO -99
#define ZERO exp(LZERO)

using namespace std;
namespace bd = breakdancer;
using boost::format;
using boost::lexical_cast;
using boost::math::cdf;
using boost::math::complement;
using boost::math::poisson_distribution;
using boost::math::chi_squared;
using boost::chrono::high_resolution_clock;
using boost::chrono::milliseconds;



namespace {
    typedef SCORE_FLOAT_TYPE real_type;

    // compute the probability score
    real_type ComputeProbScore(
            int total_region_size,
            map<size_t,int> &rlibrary_readcount,
            bd::pair_orientation_flag type,
            int fisher,
            LibraryInfo const& lib_info
            )
    {
        real_type lambda;
        real_type logpvalue = 0.0;
        real_type err = 0.0;
        for(map<size_t,int>::const_iterator ii_rlibrary_readcount = rlibrary_readcount.begin(); ii_rlibrary_readcount != rlibrary_readcount.end(); ii_rlibrary_readcount ++){
            size_t const& libindex = ii_rlibrary_readcount->first;
            int const& readcount = ii_rlibrary_readcount->second;
            LibraryConfig const& lib_config = lib_info._cfg.library_config_by_index(libindex);
            LibraryFlagDistribution const& lib_flags = lib_info._summary.library_flag_distribution_for_index(lib_config.index);

            uint32_t read_count_for_flag = lib_flags.read_counts_by_flag[type];
            lambda = real_type(total_region_size)* (real_type(read_count_for_flag)/real_type(lib_info._summary.covered_reference_length()));
            lambda = max(real_type(1.0e-10), lambda);
            poisson_distribution<real_type> poisson(lambda);
            real_type tmp_a = log(cdf(complement(poisson, readcount))) - err;
            real_type tmp_b = logpvalue + tmp_a;
            err = (tmp_b - logpvalue) - tmp_a;
            //_max_kahan_err = max(_max_kahan_err, err);
            logpvalue = tmp_b;
        }

        if(fisher && logpvalue < 0) {
            // Fisher's Method
            chi_squared chisq(2*rlibrary_readcount.size());
            try {
                real_type fisherP = cdf(complement(chisq, -2*logpvalue));
                logpvalue = fisherP > ZERO ? log(fisherP) : LZERO;
            } catch (const std::exception& e) {
                cerr << "chi squared problem: N=" << 2*rlibrary_readcount.size()
                    << ", log(p)=" << logpvalue << ", -2*log(p) = " << -2*logpvalue << "\n";
            }
        }

        return logpvalue;
    }
}

BreakDancer::BreakDancer(
        Options const& opts,
        BamConfig const& cfg,
        LibraryInfo const& lib_info,
        IBamReader& merged_reader,
        int max_read_window_size
        )
    : _opts(opts)
    , _cfg(cfg)
    , _lib_info(lib_info)
    , _merged_reader(merged_reader)
    , _max_read_window_size(max_read_window_size)

    , _collecting_normal_reads(false)
    , _nnormal_reads(0)
    , _ntotal_nucleotides(0)
    , _max_readlen(0)
    , _buffer_size(0)

    , _region_start_tid(-1)
    , _region_start_pos(-1)
    , _region_end_tid(-1)
    , _region_end_pos(-1)

{
}

void BreakDancer::run() {
    bam1_t* b = bam_init1();
    while (_merged_reader.next(b) >= 0) {
        bd::Read aln(b, _opts.need_sequence_data());

        string const& lib = _cfg.readgroup_library(aln.readgroup());
        if(!lib.empty()) {
            aln.set_lib_index(_lib_info._cfg.library_config_by_name(lib).index);
            push_read(aln, _merged_reader.header());
        }
    }
    process_final_region(_merged_reader.header());
    bam_destroy1(b);
}



void BreakDancer::push_read(bd::Read &aln, bam_header_t const* bam_header) {
    LibraryConfig const& lib_config = _lib_info._cfg.library_config_by_index(aln.lib_index());

    //main analysis code
    if(aln.bdflag() == bd::NA)
        return; // return fragment reads and other bad ones

    // min_mapping_quality is part of the bam2cfg input. I infer it is a perlibrary mapping quality cutoff

    // XXX: this value can be missing in the config (indicated by a value of -1),
    // in which case we'll wan't to use the default from the cmdline rather than
    // admit everything.
    int min_mapq = lib_config.min_mapping_quality < 0 ?
            _opts.min_map_qual : lib_config.min_mapping_quality;

    if (aln.bdqual() <= min_mapq)
        return;

    // region between last and next begin
    // Store readdepth in nread_ROI by bam name (no per library calc) or by library
    // I believe this only counts normally mapped reads
    // FIXME Weird to me that this one uses opts.min_map_qual directly
    // seems like it should use min_mapq from above. Could fix now that I've moved it
    if(aln.bdqual() > _opts.min_map_qual
        && (aln.bdflag() == bd::NORMAL_FR || aln.bdflag() == bd::NORMAL_RF))
    {
        string const& key = _opts.CN_lib == 1 ? lib_config.name : lib_config.bam_file;
        _rdata.incr_normal_read_count(key);
    }


    if ((_opts.transchr_rearrange && aln.bdflag() != bd::ARP_CTX)
            || aln.bdflag() == bd::MATE_UNMAPPED
            || aln.bdflag() == bd::UNMAPPED) // only care flag 32 for CTX
    {
        return;
    }

    //this isn't an exact match to what was here previously
    //but I believe it should be equivalent since we ignore reads are unmapped or have amate unmapped
    if(aln.bdflag() != bd::ARP_CTX && aln.abs_isize() > _opts.max_sd) {// skip read pairs mapped too distantly on the same chromosome
        return;
    }

    // for long insert
    // Mate pair libraries have different expected orientations so adjust
    // Also, aligner COULD have marked (if it was maq) that reads had abnormally large or small insert sizes
    // Remark based on BD options
    if(_opts.Illumina_long_insert){
        if(aln.abs_isize() > lib_config.uppercutoff && aln.bdflag() == bd::NORMAL_RF) {
            aln.set_bdflag(bd::ARP_RF);
        }
        if(aln.abs_isize() < lib_config.uppercutoff && aln.bdflag() == bd::ARP_RF) {
            aln.set_bdflag(bd::NORMAL_RF);
        }
        if(aln.abs_isize() < lib_config.lowercutoff && aln.bdflag() == bd::NORMAL_RF) {
            aln.set_bdflag(bd::ARP_FR_small_insert);
        }
    }
    else{
        if(aln.abs_isize() > lib_config.uppercutoff && aln.bdflag() == bd::NORMAL_FR) {
            aln.set_bdflag(bd::ARP_FR_big_insert);
        }
        if(aln.abs_isize() < lib_config.uppercutoff && aln.bdflag() == bd::ARP_FR_big_insert) {
            aln.set_bdflag(bd::NORMAL_FR);
        }
        if(aln.abs_isize() < lib_config.lowercutoff && aln.bdflag() == bd::NORMAL_FR) {
            aln.set_bdflag(bd::ARP_FR_small_insert);
        }
        if(aln.bdflag() == bd::NORMAL_RF) {
            aln.set_bdflag(bd::ARP_RF);
        }
    }
    // This makes FF and RR the same thing
    if(aln.bdflag() == bd::ARP_RR) {
        aln.set_bdflag(bd::ARP_FF);
    }

    //count reads mapped by SW, FR and RF reads, but only if normal_switch is true
    //normal_switch is set to 1 as soon as reads are accumulated for dumping to fastq??? Not sure on this. Happens later in this function
    //I suspect this is to include those reads in the fastq dump for assembly!
    if(aln.bdflag() == bd::NORMAL_FR || aln.bdflag() == bd::NORMAL_RF) {
        if(_collecting_normal_reads && aln.isize() > 0){
            ++_nnormal_reads;
        }
        return;
    }

    if(_collecting_normal_reads){
        _ntotal_nucleotides += aln.query_length();
        _max_readlen = std::max(_max_readlen, aln.query_length());
    }

    //This appears to test that you've exited a window after your first abnormal read by either reading off the chromosome or exiting the the window
    // d appears to be 1e8 at max (seems big), 50 at minimum or the smallest mean - readlen*2 for a given library
    bool do_break = aln.tid() != _region_end_tid || aln.pos() - _region_end_pos > _max_read_window_size;

    if(do_break) { // breakpoint in the assembly
        process_breakpoint(bam_header);
        // clear out this node
        _region_start_tid = aln.tid();
        _region_start_pos = aln.pos();
        reads_in_current_region.clear();
        _collecting_normal_reads = false;
        _nnormal_reads = 0;
        _max_readlen = 0;
        _ntotal_nucleotides = 0;

        _rdata.clear_region_accumulator();
        _rdata.clear_flanking_region_accumulator();
    }

    reads_in_current_region.push_back(aln); // store each read in the region_sequence buffer
    //
    //If we just added the first read, flip the flag that lets us collect all reads
    if(reads_in_current_region.size() == 1)
        _collecting_normal_reads = true;
    _region_end_tid = aln.tid();
    _region_end_pos = aln.pos();

    _rdata.clear_region_accumulator();

    return;
}

void BreakDancer::process_breakpoint(bam_header_t const* bam_header) {
    ScopedTimer<high_resolution_clock, milliseconds> timer(cerr, "process_breakpoint");

    float seq_coverage = _ntotal_nucleotides/float(_region_end_pos - _region_start_pos + 1 + _max_readlen);
    if(_region_end_pos - _region_start_pos > _opts.min_len
            && seq_coverage < _opts.seq_coverage_lim) // skip short/unreliable flanking supporting regions
    {
        // register reliable region and supporting reads across gaps
        //int region_idx = _rdata.add_region(new BasicRegion(_region_start_tid, _region_start_pos, _region_end_pos, _nnormal_reads));
        //add_current_read_counts_to_last_region();
        _rdata.add_region(_region_start_tid, _region_start_pos, _region_end_pos, _nnormal_reads, reads_in_current_region);

        ++_buffer_size; //increment tracking of number of regions in buffer???
        if(_buffer_size > _opts.buffer_size){
            build_connection(bam_header);
            //flush buffer by building connection
            _buffer_size = 0;
        }
    }
    else {
        _rdata.collapse_accumulated_data_into_last_region(reads_in_current_region);
    }
}



void BreakDancer::build_connection(bam_header_t const* bam_header) {
    ScopedTimer<high_resolution_clock, milliseconds> timer(cerr, "build_connection");
    // build connections
    // find paired regions that are supported by paired reads

    typedef ReadRegionData::Subgraph Subgraph;
    typedef ReadRegionData::Graph Graph;
    Graph graph(_rdata.region_graph());

    // segregate graph, find nodes that have connections
    set<int> free_nodes;
    Graph::iterator ii_graph = graph.begin();

    while (ii_graph != graph.end()) {
        vector<int> tails;
        tails.push_back(ii_graph->first);
        bool need_iter_increment = true;
        while(tails.size() > 0) {
            vector<int> newtails;
            vector<int>::const_iterator it_tails;
            for(it_tails = tails.begin(); it_tails != tails.end(); ++it_tails) {
                int const& tail = *it_tails;

                // Make sure region with id "tail" hasn't already been deleted
                assert(_rdata.region_exists(tail));
                if(!_rdata.region_exists(tail))
                    continue;

                //assert(graph.find(tail) != graph.end()); THIS ASSERT TRIPS
                Graph::iterator found = graph.find(tail);
                if (found == graph.end())
                    continue;

                Subgraph& graph_tail = found->second;
                Subgraph::iterator ii_graph_tail = graph_tail.begin();
                while (ii_graph_tail != graph_tail.end()) {
                    int s1 = ii_graph_tail->first;
                    int nlinks = ii_graph_tail->second;

                    graph_tail.erase(ii_graph_tail++);

                    assert(_rdata.region_exists(s1));
                    // require sufficient number of pairs
                    if(nlinks < _opts.min_read_pair || !_rdata.region_exists(s1)) {
                        continue;
                    }

                    vector<int> snodes;
                    if(tail != s1) {
                        graph[s1].erase(tail);
                        snodes.push_back(std::min(s1, tail));
                        snodes.push_back(std::max(s1, tail));
                    }
                    else
                        snodes.push_back(s1);

                    newtails.push_back(s1);
                    process_sv(snodes, free_nodes, bam_header);
                }
                if (tail == ii_graph->first) {
                    // The fact that this is postincrement is critical
                    graph.erase(ii_graph++);
                    need_iter_increment = false;
                } else {
                    graph.erase(tail);
                }
            }
            tails.swap(newtails);
        }
        if (need_iter_increment)
            ++ii_graph;
    }

    // free regions
    for(set<int>::const_iterator i = free_nodes.begin(); i != free_nodes.end(); ++i) {
        // Hey, is it just me or does the following comparison double count
        // reads with mates in the same region and then go on to compare that
        // quantity to something measured in pairs?
        //
        // -ta
        if(_rdata.num_reads_in_region(*i) < unsigned(_opts.min_read_pair))
            _rdata.clear_region(*i);
    }
}

void BreakDancer::process_sv(std::vector<int> const& snodes, std::set<int>& free_nodes, bam_header_t const* bam_header) {
    typedef ReadRegionData::read_iter_range ReadRange;
    BasicRegion const* regions[2] = {0};
    ReadRange read_ranges[2];
    for (size_t i = 0; i < snodes.size(); ++i) {
        int const& region_idx = snodes[i];
        regions[i] = &_rdata.region(region_idx);
        read_ranges[i] = _rdata.region_reads_range(region_idx);
    }
    SvBuilder svb(snodes.size(), regions, read_ranges, _max_readlen);

    // This predicate takes a read and evaluates:
    //      read_pair.count(read.query_name()) == 0
    using boost::bind;
    boost::function<bool(ReadType const&)> is_supportive = bind(
        std::equal_to<size_t>(), 0, bind(&SvBuilder::ObservedReads::count,
            &svb.observed_reads, bind(&ReadType::query_name, _1)));

    for (vector<int>::const_iterator i = snodes.begin(); i != snodes.end(); ++i)
        _rdata.remove_reads_in_region_if(*i, is_supportive);

    if(svb.num_pairs < _opts.min_read_pair)
        return;

    assert(snodes.size() == 1 || snodes.size() == 2);
    if(svb.flag_counts[svb.flag] >= _opts.min_read_pair) {
        int diffspan;
        string sptype;

        // print out result
        ReadCountsByLib read_count_accumulator;
        if (snodes.size() == 2) {
            _rdata.accumulate_reads_between_regions(read_count_accumulator, snodes[0], snodes[1]);
        }
        svb.compute_copy_number(read_count_accumulator, read_density);


        if(svb.flag != bd::ARP_RF && svb.flag != bd::ARP_RR && svb.pos[0] + _max_readlen - 5 < svb.pos[1])
            svb.pos[0] += _max_readlen - 5; // apply extra padding to the start coordinates

        string sptype_tmp;
        float diff = 0;
        if(_opts.CN_lib == 1){
            for(map<size_t,int>::const_iterator ii_type_lib_rc = svb.type_library_readcount[svb.flag].begin(); ii_type_lib_rc != svb.type_library_readcount[svb.flag].end(); ii_type_lib_rc ++){
                size_t const& index = ii_type_lib_rc->first;
                int const& read_count = ii_type_lib_rc->second;
                LibraryConfig const& lib_config = _lib_info._cfg.library_config_by_index(index);
                // intialize to be zero, in case of no library, or DEL, or ITX.

                string copy_number_str = "NA";
                if(svb.flag != bd::ARP_CTX){
                    float copy_number_ = 0;

                    if(svb.copy_number.find(lib_config.name) != svb.copy_number.end()){
                        copy_number_ = svb.copy_number[lib_config.name];
                        stringstream sstr;
                        sstr << fixed;
                        sstr << setprecision(2) << copy_number_;
                        copy_number_str = sstr.str();
                    }
                }
                if(!sptype_tmp.empty())
                    sptype_tmp += ":";

                sptype_tmp += lib_config.name + "|" + lexical_cast<string>(read_count) + "," + copy_number_str;

                diff += float(svb.type_library_meanspan[svb.flag][index]) - float(svb.type_library_readcount[svb.flag][index])*lib_config.mean_insertsize;
            }
        } // do lib for copy number and support reads
        else{
            map<string, int> type_bam_readcount;
            for(map<size_t, int>::const_iterator ii_type_lib_rc = svb.type_library_readcount[svb.flag].begin(); ii_type_lib_rc != svb.type_library_readcount[svb.flag].end(); ii_type_lib_rc ++){
                size_t const& index = ii_type_lib_rc->first;
                int const& read_count = ii_type_lib_rc->second;
                LibraryConfig const& lib_config = _lib_info._cfg.library_config_by_index(index);
                type_bam_readcount[lib_config.bam_file] += read_count;
                diff += float(svb.type_library_meanspan[svb.flag][index]) - float(svb.type_library_readcount[svb.flag][index])*lib_config.mean_insertsize;
            }
            for(map<string, int>::const_iterator ii_type_bam_rc = type_bam_readcount.begin(); ii_type_bam_rc != type_bam_readcount.end(); ii_type_bam_rc ++){
                string const& sp = ii_type_bam_rc->first;
                if(!sptype_tmp.empty())
                    sptype_tmp += ":";
                sptype_tmp += sp + "|" + lexical_cast<string>((*ii_type_bam_rc).second);
            }
            if(sptype_tmp.empty()) {
                sptype_tmp = "NA";
            }
        } // do bam for support reads; copy number will be done later

        diffspan = int(diff/float(svb.flag_counts[svb.flag]) + 0.5);
        sptype = sptype_tmp;


        int total_region_size = _rdata.sum_of_region_sizes(snodes);
        real_type LogPvalue = ComputeProbScore(total_region_size, svb.type_library_readcount[svb.flag], svb.flag, _opts.fisher, _lib_info);
        real_type PhredQ_tmp = -10*LogPvalue/log(10);
        int PhredQ = PhredQ_tmp>99 ? 99:int(PhredQ_tmp+0.5);

        string SVT = _opts.SVtype.find(svb.flag)==_opts.SVtype.end()?"UN":_opts.SVtype.at(svb.flag); // UN stands for unknown
        // Convert the coordinates to base 1
        ++svb.pos[0];
        ++svb.pos[1];
        if(PhredQ > _opts.score_threshold){
            cout << bam_header->target_name[svb.chr[0]]
                << "\t" << svb.pos[0]
                << "\t" << svb.fwd_read_count[0] << "+" << svb.rev_read_count[0] << "-"
                << "\t" << bam_header->target_name[svb.chr[1]]
                << "\t" << svb.pos[1]
                << "\t" << svb.fwd_read_count[1] << "+" << svb.rev_read_count[1] << "-"
                << "\t" << SVT
                << "\t" << diffspan
                << "\t" << PhredQ
                << "\t" << svb.flag_counts[svb.flag]
                << "\t" << sptype
                ;

            if(_opts.print_AF == 1)
                cout <<  "\t" << svb.allele_frequency;

            if(_opts.CN_lib == 0 && svb.flag != bd::ARP_CTX){
                vector<string> const& bams = _cfg.bam_files();
                for(vector<string>::const_iterator iter = bams.begin(); iter != bams.end(); ++iter) {
                    map<string, float>::const_iterator cniter = svb.copy_number.find(*iter);

                    if(cniter  == svb.copy_number.end())
                        cout << "\tNA";
                    else {
                        cout << "\t";
                        cout << fixed;
                        cout << setprecision(2) << cniter->second;
                    }
                }
            }
            cout << "\n";


            if(!_opts.prefix_fastq.empty()){ // print out supporting read pairs
                dump_fastq(svb.flag, svb.support_reads);
            }

            if(!_opts.dump_BED.empty()){  // print out SV and supporting reads in BED format
                ofstream fh_BED(_opts.dump_BED.c_str(), ofstream::app);

                string trackname(bam_header->target_name[svb.chr[0]]);
                trackname = trackname.append("_").append(lexical_cast<string>(svb.pos[0])).append("_").append(SVT).append("_").append(lexical_cast<string>(diffspan));
                fh_BED << "track name=" << trackname << "\tdescription=\"BreakDancer" << " " << bam_header->target_name[svb.chr[0]] << " " << svb.pos[0] << " " << SVT << " " << diffspan << "\"\tuseScore=0\n";
                for(vector<bd::Read>::const_iterator ii_support_reads = svb.support_reads.begin(); ii_support_reads != svb.support_reads.end(); ii_support_reads ++){
                    bd::Read const& y = *ii_support_reads;
                    if(y.query_sequence().empty() || y.quality_string().empty() || y.bdflag() != svb.flag)
                        continue;
                    int aln_end = y.pos() - y.query_length() - 1;
                    string color = y.ori() == FWD ? "0,0,255" : "255,0,0";
                    //FIXME if the bam already used chr prefixed chromosome names this would duplicate them...
                    fh_BED << "chr" << bam_header->target_name[y.tid()]
                        << "\t" << y.pos()
                        << "\t" << aln_end
                        << "\t" << y.query_name() << "|" << _lib_info._cfg.library_config_by_index(y.lib_index()).name
                        << "\t" << y.bdqual() * 10
                        << "\t" << y.ori()
                        << "\t" << y.pos()
                        << "\t" << aln_end
                        << "\t" << color
                        << "\n";
                }
                fh_BED.close();
            }
        }
    }

    std::for_each(svb.reads_to_free.begin(), svb.reads_to_free.end(),
        boost::bind(&ReadRegionData::erase_read, &_rdata, _1));

    free_nodes.insert(snodes.begin(), snodes.end());
}

void BreakDancer::dump_fastq(breakdancer::pair_orientation_flag const& flag, std::vector<ReadType> const& support_reads) {
    map<string,int> pairing;
    for( vector<bd::Read>::const_iterator ii_support_reads = support_reads.begin(); ii_support_reads != support_reads.end(); ii_support_reads ++){
        bd::Read const& y = *ii_support_reads;

        if(y.query_sequence().empty() || y.quality_string().empty() || y.bdflag() != flag)
            continue;

        //Paradoxically, the first read seen is put in file 2 and the second in file 1
        string suffix = pairing.count(y.query_name()) ? "1" : "2";
        string fh_tmp_str = _cfg.ReadsOut.at(_lib_info._cfg.library_config_by_index(y.lib_index()).name + suffix);
        _fastq_writer.write(fh_tmp_str, y);
        pairing[y.query_name()] = 1;
    }
}

void BreakDancer::process_final_region(bam_header_t const* bam_header) {
   if (reads_in_current_region.size() != 0) {
        process_breakpoint(bam_header);
    }
    build_connection(bam_header);
}
