#include <omp.h>
#include <mutex>
#include <math.h>
#include <string>
#include <stdio.h>
#include <fstream>
#include <iomanip>
#include <assert.h>
#include <iostream>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>
#include <bits/stdc++.h>

#include "stats.hpp"
#include "logger.hpp"
#include "counter.hpp"
#include "genotyper.hpp"

#include "json.hpp"

#ifdef LINEARSOL
#include "ortools/linear_solver/linear_solver.h"
using namespace absl ;
namespace lp = operations_research ;
#endif

using namespace std ;

// ============================================================================= \\
// ============================================================================= \\
// ============================================================================= \\

double calc_mean(vector<int> counts) {
    return accumulate(counts.begin(), counts.end(), 0) / counts.size() ;
}

double calc_std(vector<int> counts, double mean) {
    double sum = 0 ;
    for (auto count: counts) {
        sum += (count - mean) * (count - mean) ;
    }
    double var = sum / counts.size() ;
    return sqrt(var) ;
}

std::string GENOTYPES[] = {"0/0", "0/1", "1/1"} ;

// ============================================================================= \\
// ============================================================================= \\
// ============================================================================= \\

void Genotyper::run() {
    auto c = Configuration::getInstance() ;
    load_kmers() ;
    if (c->reduce) {
        cout << "Loading counted kmers.." << endl ;
        load_counts() ;
    } else {
        count_kmers() ;
    }
    estimate_coverage() ;
    adjust_gc_coverage() ;
    if (c->select) {
        filter_kmers() ;
        remove_non_unique_kmers() ;
    }
    cluster_kmers() ;
    genotype_clusters() ;
    export_genotypes() ;
}

void Genotyper::load_kmers() {
    auto c = Configuration::getInstance() ;
    cout << "loading gc kmers.. " << c->gc_kmers << endl ;
    ifstream json_file(c->gc_kmers) ;
    nlohmann::json kmers_json ;
    cout << "dumping file into kmers_json"<< endl ;
    json_file >> kmers_json ;
    cout << "dumped file into kmers_json"<< endl ;
    for (nlohmann::json::iterator kmer = kmers_json.begin(); kmer != kmers_json.end(); kmer++) {
        uint64_t k = encode_kmer(canonicalize(kmer.key()).c_str()) ;
        Kmer _kmer ;
        _kmer.gc = kmer.value().at("gc").get<int>() ;
        _kmer.seq = k ;
        gc_kmers.emplace(std::make_pair(k, _kmer)) ;
    }
    json_file.close() ;
    cout << "loading depth kmers.." << endl ;
    json_file.open(c->depth_kmers) ;
    nlohmann::json depth_kmers_json ;
    json_file >> depth_kmers_json ;
    for (nlohmann::json::iterator kmer = depth_kmers_json.begin(); kmer != depth_kmers_json.end(); kmer++) {
        uint64_t k = encode_kmer(canonicalize(kmer.key()).c_str()) ;
        Kmer _kmer ;
        _kmer.seq = k ;
        depth_kmers.emplace(std::make_pair(k, _kmer)) ;
    }
    json_file.close() ;
    cout << "loading genotyping kmers.." << endl ;
    int inner ;
    int junction ;
    int num_batches = 100 ;
    vector<unordered_map<uint64_t, Kmer>> _kmers(num_batches) ;
    #pragma omp parallel for num_threads(c->threads)
    for (int i = 0; i < num_batches; i++) {
        int t = i ;
        ifstream json_file ;
        string path = c->kmers[0] + "/kmers_batch_" + std::to_string(t) + ".json" ;
        struct stat info ;
        if (stat(path.c_str(), &info) != 0) {
            continue ;
        }
        json_file.open(path) ;
        nlohmann::json kmers_json ;
        json_file >> kmers_json ;
        for (nlohmann::json::iterator kmer = kmers_json.begin(); kmer != kmers_json.end(); ++kmer) {
            uint64_t k = encode_kmer(kmer.key()) ;
            _kmers[t][k] = kmer.value().get<Kmer>() ;
            _kmers[t][k].seq = encode_kmer(kmer.key()) ;
            _kmers[t][k].count = 0 ; 
            _kmers[t][k].total = 0 ;
            if (_kmers[t][k].type == KMER_TYPE_JUNCTION) {
                junction += 1 ;
            } else {
                inner += 1 ;
            }
        }
        json_file.close() ;
    }
    cout << "Loaded all kmers batches, merging.." << endl ;
    for (int i = 0; i < num_batches; i++) {
        kmers.insert(_kmers[i].begin(), _kmers[i].end()) ;
        _kmers[i].clear() ;
    }
    cout << "Loaded all kmers:" << endl ;
    cout << "\t" << gc_kmers.size() << " GC kmers." << endl ;
    cout << "\t" << depth_kmers.size() << " depth kmers." << endl ;
    cout << "\t" << kmers.size() << " genotyping kmers." << endl ;
    cout << "\t\t" << inner << " inner kmers." << endl ;
    cout << "\t\t" << junction << " junction kmers." << endl ;
}

void Genotyper::count_kmers() {
    auto counter = KmerCounter(16, &depth_kmers, &gc_kmers, &kmers) ;
    counter.run() ;
}

void Genotyper::load_counts() {
    auto counter = KmerCounter(16, &depth_kmers, &gc_kmers, &kmers) ;
    counter.load_counts() ;
}

void Genotyper::estimate_coverage() {
    std::vector<int> counts ;
    for (auto kmer = depth_kmers.begin(); kmer != depth_kmers.end(); kmer++) {
        counts.push_back(kmer->second.count) ;
    }
    this->coverage = calc_mean(counts) ;
    while (true) {
        this->std = calc_std(counts, coverage) ;
        counts.erase(std::remove_if(counts.begin(), counts.end(),
            [this](const int & o) { return o > this->coverage + 3 * this->std ; }), counts.end()) ;
        double m = calc_mean(counts) ;
        cout << "Coverage " << this->coverage << "x with variance " << std << "." << endl ;
        if (abs(this->coverage - m) < 0.5) {
            break ;
        }
        this->coverage = m ;
    }
    cout << "Coverage " << this->coverage << "x with variance " << std << "." << endl ;
}

void Genotyper::adjust_gc_coverage() {
    vector<int> counts ;
    vector<vector<int>> gc(101) ;
    for (auto kmer = gc_kmers.begin(); kmer != gc_kmers.end(); kmer++) {
        gc[kmer->second.gc].push_back(kmer->second.count) ;
    }
    for (int i = 0; i <= 100; i++) {
        if (gc[i].size() > 0) {
            while (true) {
                double _mean = calc_mean(gc[i]) ;
                double _std = calc_std(gc[i], _mean) ;
                gc[i].erase(std::remove_if(gc[i].begin(), gc[i].end(),
                    [_mean, _std](const int & o) { return o > _mean + 3 * _std ; }), gc[i].end()) ;
                double m = calc_mean(gc[i]) ;
                if (abs(_mean - m) < 0.5) {
                    gc_coverage.push_back(max(0.2 * coverage, _mean)) ;
                    break ;
                }
            }
        } else {
            gc_coverage.push_back(coverage) ;
        }
        //cout << gc_coverage[gc_coverage.size() - 1] << endl ;
    }
}

double Genotyper::calc_kmer_genotype_likelihood(Kmer& kmer, std::vector<string> genotypes) {
    auto c = Configuration::getInstance() ;
    double std = 0 ;
    double mean = 0 ;
    double max_count = 0 ;
    int i = 0 ;
    for (auto track = kmer.tracks.begin(); track != kmer.tracks.end(); track++) {
        //auto genotype = c->tracks[track->first].genotype ;
        auto genotype = genotypes[i] ;
        genotype = genotype == "1/0" ? "0/1" : genotype ;
        double m = genotype == "0/0" ? 0.0 : genotype == "0/1" ? 0.5 : 1.0 ;
        double s = genotype == "0/0" ? 0.25 : genotype == "0/1" ? 0.5 : 1.0 ;
        // find the locus for this track
        bool found = false ;
        for (auto locus: kmer.loci) {
            if (kmer.type == KMER_TYPE_JUNCTION && locus.type == LOCUS_TYPE_JUNCTION && locus.position == track->first.begin) {
                std += s * this->std ;
                mean += m * this->coverage ;
                max_count += this->coverage ;
                found = true ;
                break ;
            }
            if (kmer.type == KMER_TYPE_INNER && locus.type == LOCUS_TYPE_INNER && locus.position >= track->first.begin - 32 && locus.position <= track->first.end) {
                if (locus.trend) {
                    std += s * this->std ;
                    mean += m * gc_coverage[locus.gc] ;
                    max_count += gc_coverage[locus.gc] ; 
                } else {
                    std += (0.25 / s) * this->std ;
                    mean += (1 - m) * gc_coverage[locus.gc] ;
                    max_count += gc_coverage[locus.gc] ; 
                }
                found = true ;
                break ;
            }
        }
        // if inverse, then onlu ref loci
        if (!found) {
            if (!kmer.inverse) {
                print_kmer(kmer) ;
            }
            assert(kmer.inverse) ;
        }
        i += 1 ;
    }
    if (kmer.inverse) {
        std = 0 ;
        mean = 0 ;
        // assumes one loci for each track
        int i = 0 ;
        for (auto track = kmer.tracks.begin(); track != kmer.tracks.end(); track++) {
            auto genotype = genotypes[i] ;
            genotype = genotype == "1/0" ? "0/1" : genotype ;
            double m = genotype == "0/0" ? 0.00  : genotype == "0/1" ? 0.5 : 1.0 ;
            double s = genotype == "0/0" ? 0.25  : genotype == "0/1" ? 0.5 : 1.0 ;
            std += s * this->std ;
            mean += m * this->coverage ;
            max_count += this->coverage ;
            i += 1 ;
        }
    } else {
        for (auto locus: kmer.loci) {
            if (locus.type == LOCUS_TYPE_REF) {
                std += this->std ;
                mean += this->coverage ;
                max_count += this->coverage ;
            }
        }
    }
    int count = kmer.inverse ? kmer.total - kmer.count : kmer.count ;
    int lp_count = min(count, int(max_count)) ;
    return NormalDistribution(mean, std).log_prob(lp_count) ;
}

void Genotyper::filter_kmer(Kmer& kmer, unordered_map<SimpleTrack, uint8_t>::iterator it, vector<string>* genotypes, double& max_likelihood, vector<string>* choice) {
    if (it == kmer.tracks.end()) {
        auto likelihood = calc_kmer_genotype_likelihood(kmer, *genotypes) ;
        //cout << likelihood << endl ;
        if (likelihood > max_likelihood) {
            max_likelihood = likelihood ;
            choice->clear() ;
            choice->insert(choice->begin(), genotypes->begin(), genotypes->end()) ;
            assert(choice->size() == kmer.tracks.size()) ;
        }
    } else {
        for (auto g: GENOTYPES) {
            //cout << g << endl ;
            genotypes->push_back(g) ;
            //cout << genotypes->size() << endl ;
            auto _it = it ;
            _it++ ;
            filter_kmer(kmer, _it, genotypes, max_likelihood, choice) ;
            genotypes->pop_back() ;
        }
    }
}

void Genotyper::filter_kmers() {
    auto c = Configuration::getInstance() ;
    int e = 0 ;
    int threads = min(c->threads, int(kmers.bucket_count())) ;
    vector<vector<uint64_t>> filters(threads) ;
    cout << "Filtering " << kmers.size() << " kmers using " << threads << " threads.." << endl ;
    if (c->unique) {
        cout << "Only unique kmers.." << endl ;
    }
    #pragma omp parallel for num_threads(threads)
    for(size_t b = 0; b < kmers.bucket_count(); b++) {
        for(auto it = kmers.begin(b); it != kmers.end(b); it++) {
            auto& kmer = it->second ;
            int t = omp_get_thread_num() ;
            //print_kmer(kmer) ;
            assert(kmer.loci.size() >= 1) ;
            if (c->unique) {
                if (kmer.loci.size() != 1) {
                    filters[t].push_back(it->first) ;
                    continue ;
                }
            } else {
                if (kmer.loci.size() > 3) {
                    filters[t].push_back(it->first) ;
                    continue ;
                }
            }
            assert(kmer.tracks.size() >= 1) ;
            if (c->unique) {
                if (kmer.tracks.size() != 1) {
                    filters[t].push_back(it->first) ;
                    continue ;
                }
            } else {
                if (kmer.tracks.size() > 2) {
                    filters[t].push_back(it->first) ;
                    continue ;
                }
            }
            bool found = false ;
            for (auto track = kmer.tracks.begin(); track != kmer.tracks.end(); track++) {
                if (track->second > 1) {
                    found = true ;
                }
            }
            if (found) {
                filters[t].push_back(it->first) ;
                continue ;
            }
            //TODO: remove
            //if (kmer.loci.size() == 1) {
            //    filters[t].push_back(it->first) ;
            //    continue ;
            //}
            //if (kmer.tracks.size() == 1) {
            //    filters[t].push_back(it->first) ;
            //    continue ;
            //}
            //print_kmer(kmer) ;
            vector<string> choice ;
            vector<string> genotypes ;
            double max_likelihood = -100000 ;
            filter_kmer(kmer, kmer.tracks.begin(), &genotypes, max_likelihood, &choice) ;
            assert(choice.size() == kmer.tracks.size()) ;
            int i = 0 ;
            bool pass = true ;
            for(auto track = kmer.tracks.begin(); track != kmer.tracks.end(); track++) {
                if (choice[i].find("1") != string::npos) {
                    if (c->tracks.find(track->first) != c->tracks.end()) {
                        if (c->tracks.find(track->first)->second.genotype.find("1") != string::npos) {
                            pass = pass && true ;
                        } else {
                            pass = pass && false ;
                        }
                    } else {
                        pass = pass && false ;
                    }
                } else { // choice is "0/0"
                    if (c->tracks.find(track->first) == c->tracks.end()) {
                        pass = pass && true ;
                    } else if (c->tracks.find(track->first)->second.genotype.find("1") == string::npos) {
                        pass = pass && true ;
                    } else {
                        pass = pass && false ;
                    }
                }
                i++ ;
            }
            if (!pass) {
                filters[t].push_back(it->first) ;
                continue ;
            }
        }
    }
    for (auto it = filters.begin(); it != filters.end(); it++) {
        for (auto k: *it) {
            kmers.erase(kmers.find(k)) ;
        }
    }
    int inner = 0 ;
    int junction = 0 ;
    for (auto kmer = kmers.begin(); kmer != kmers.end(); kmer++) {
        if (kmer->second.type == KMER_TYPE_INNER) {
            inner += 1 ;
        } else {
            junction += 1 ;
        }
    }
    cout << kmers.size() << " kmers remain after filtering:" << endl ;
    cout << "\tInner kmers: " << inner << endl ;
    cout << "\tJunction kmers: " << junction << endl ;
    cout << "\tCorrupted kmers: " << e << endl ;
}

void Genotyper::remove_non_unique_kmers() {
    auto c = Configuration::getInstance() ;
    for (auto kmer = kmers.begin(); kmer != kmers.end(); kmer++) {
        for (auto track = kmer->second.tracks.begin(); track != kmer->second.tracks.end(); track++) {
            tracks[track->first].push_back(kmer->first) ;
        }
    }
    // find tracks with only non-unique kmers
    unordered_map<SimpleTrack, bool> non_unique_tracks ;
    for (auto track = tracks.begin(); track != tracks.end(); track++) {
        auto a = find_if(track->second.begin(), track->second.end(), [this](uint64_t k) {
            return this->kmers[k].tracks.size() == 1 ;
        }) ;
        if (a == track->second.end()) {
            non_unique_tracks[track->first] = true ;
        }
    }
    cout << non_unique_tracks.size() << " tracks with only non-unique kmers" << endl ;
    int threads = min(c->threads, int(kmers.bucket_count())) ;
    vector<vector<uint64_t>> filters(threads) ;
    #pragma omp parallel for num_threads(threads)
    for(size_t b = 0; b < kmers.bucket_count(); b++) {
        for(auto it = kmers.begin(b); it != kmers.end(b); it++) {
            auto kmer = it->second ;
            int t = omp_get_thread_num() ;
            if (kmer.tracks.size() > 1) {
                bool found = false ;
                for (auto track = kmer.tracks.begin(); track != kmer.tracks.end(); track++) {
                    if (non_unique_tracks.find(track->first) != non_unique_tracks.end()) {
                        found = true ;
                        break ;
                    }
                }
                if (!found) {
                    filters[t].push_back(it->first) ;
                }
            }
        }
    }
    int n = 0 ;
    for (auto it = filters.begin(); it != filters.end(); it++) {
        for (auto k: *it) {
            kmers.erase(kmers.find(k)) ;
            n += 1 ;
        }
    }
    cout << "Removed " << n << " non-unique kmers." << endl ;
}

void Genotyper::cluster_kmers() {
    auto c = Configuration::getInstance() ;
    if (c->select) {
        cout << "Clustering " << tracks.size() << " tracks with " << kmers.size() << " kmers.." << endl ;
        tracks.clear() ;
    }
    for (auto kmer = kmers.begin(); kmer != kmers.end(); kmer++) {
        for (auto track = kmer->second.tracks.begin(); track != kmer->second.tracks.end(); track++) {
            tracks[track->first].push_back(kmer->first) ;
        }
    }
    cout << "Clustering " << tracks.size() << " tracks with " << kmers.size() << " kmers.." << endl ;
    for (auto track = tracks.begin(); track != tracks.end(); track++) {
        cluster_index[track->first] = -1 ;
    }
    for (auto it = tracks.begin(); it != tracks.end(); it++) {
        auto track = it->first ;
        int cluster = cluster_index[track] ;
        for (auto k = it->second.begin(); k != it->second.end(); k++) {
            for (auto _it = kmers[*k].tracks.begin(); _it != kmers[*k].tracks.end(); _it++) {
                auto _track = _it->first ;
                int _cluster = cluster_index[_track] ;
                if (track != _track) { // look at other tracks only
                    if (_cluster == -1) { // other track not yet clustered
                        if (cluster == -1) { // current track not yet clustered
                            cluster = clusters.size() ;
                            clusters.push_back({track, _track}) ;
                            cluster_index[track] = cluster ;
                            cluster_index[_track] = cluster ;
                        } else {
                            continue ;
                            // current track clustered, will add other track later when it's its turn in the loop
                        }
                    } else { // append current track to cluster
                        if (cluster == -1) {
                            cluster = _cluster ;
                            cluster_index[track] = _cluster ;
                            clusters[_cluster].push_back(track) ; 
                        } else {
                            if (cluster != _cluster) {
                                for (auto __track: clusters[_cluster]) {
                                    cluster_index[__track] = cluster ;
                                    clusters[cluster].push_back(__track) ;
                                }
                                clusters[_cluster].clear() ;
                            }
                        }
                    }
                }
            }
        }
        if (cluster == -1) {
            cluster = clusters.size() ;
            clusters.push_back({track}) ;
            cluster_index[track] = cluster ;
        }
    }
    int p = 0 ;
    int m = 0 ;
    for (auto it = clusters.begin(); it != clusters.end(); it++) {
        if ((*it).size() > 0) {
            m += 1 ;
        }
        if ((*it).size() > 1) {
            p += 1 ;
        }
    }
    cout << tracks.size() << " tracks with " << clusters.size() << " clusters." << endl ;
    cout << m << " singular clusters." << endl ;
    cout << p << " composite clusters." << endl ;
    m = 0 ;
    for (auto track = tracks.begin(); track != tracks.end(); track++) {
        m += track->second.size() ;
        genotyping_tracks[track->first].lp_value = 0.0 ;
    }
    cout << m << " kmers in clusters." << endl ;
    cout << "Validating kmers just in case.." << endl ;
    for (auto kmer = kmers.begin(); kmer != kmers.end(); kmer++) {
        assert(kmer->second.loci.size() >= 1) ;
        assert(kmer->second.tracks.size() >= 1) ;
    }
}

void Genotyper::genotype_clusters() {
    cout << "Genotyping.." << endl ;
    auto c = Configuration::getInstance() ;
    int n = 0 ;
    int batch_size = 1 ;
    #pragma omp parallel for num_threads(48)
    for (int i = 0; i < clusters.size() / batch_size; i++) {
        if (clusters[i * batch_size].size() == 1) {
            genotype_cluster_likelihood(i * batch_size, (i + 1) * batch_size) ;
        } else {
            #ifdef LINEARSOL
            genotype_cluster_lp(i * batch_size, (i + 1) * batch_size) ;
            #endif
        }
        n += 1 ;
        if (n % 100 == 0) {
            cout << "Solved " << std::left << std::setw(6) << n << " out of " << std::setw(6) << clusters.size() / batch_size << " batches.\r" ;
            cout << endl ;
        }
    }
    cout << "\n" ;
}

void Genotyper::genotype_cluster_likelihood(int batch_b, int batch_e) {
    auto c = Configuration::getInstance() ;
    if (clusters[batch_b].size() == 0) {
        return ;
    }
    unordered_map<SimpleTrack, int> track_index ;
    for (int i = batch_b; i < batch_e; i++) {
        for (auto track = clusters[i].begin(); track != clusters[i].end(); track++) {
            if (tracks.find(*track) != tracks.end()) {
                track_index[*track] = track_index.size() ;
            }
        }
    }
    int u = 0 ;
    unordered_map<string, double> likelihoods ;
    likelihoods["0/0"] = 0.0 ;
    likelihoods["0/1"] = 0.0 ;
    likelihoods["1/1"] = 0.0 ;
    unordered_map<uint64_t, bool> lp_kmers ;
    auto name = clusters[batch_b][0].get_name() ;
    ofstream lp_file(c->workdir + "/batch_" + name + ".model") ;
    ofstream json_file(c->workdir + "/batch_" + name + ".json") ;
    json_file << "{\n" ;
    for (int i = batch_b; i < batch_e; i++) {
        for (auto track = clusters[i].begin(); track != clusters[i].end(); track++) {
            for (auto k = tracks[*track].begin(); k != tracks[*track].end(); k++) {
                if (lp_kmers.find(*k) == lp_kmers.end()) {
                    lp_kmers[*k] = true ;
                    auto kmer = kmers[*k] ;
                    int count = kmer.inverse ? kmer.total - kmer.count : kmer.count ;
                    lp_file << decode_kmer(*k) << " " << count << endl ;
                    if (u != 0) {
                        json_file << ",\n" ;
                    }
                    json_file << "\"" << decode_kmer(*k) << "\":" ;
                    json_file << nlohmann::json(kmer).dump(4) ;
                    u += 1 ;
                    for (auto g: GENOTYPES) {
                        auto l = calc_kmer_genotype_likelihood(kmer, {g}) ;
                        likelihoods[g] += l ;
                        lp_file << g << ": " << l << endl ;
                    }
                }
            }
        }
    }
    json_file << "\n}\n" ;
    for (auto track = track_index.begin(); track != track_index.end(); track++) {
        string choice = "0/0" ;
        double max_likelihood = 1 ;
        for (auto g: GENOTYPES) {
            if (likelihoods[g] > max_likelihood || max_likelihood > 0) {
                max_likelihood = likelihoods[g] ;
                choice = g ;
            }
        }
        genotyping_tracks[track->first].lp_value = choice == "1/1" ? 1.0 : choice == "0/1" ? 0.5 : 0.0 ; 
    }
}

#ifdef LINEARSOL
void Genotyper::genotype_cluster_lp(int batch_b, int batch_e) {
    auto c = Configuration::getInstance() ;
    if (clusters[batch_b].size() == 0) {
        return ;
    }
    lp::MPSolver solver("cluster_" + std::to_string(batch_b), lp::MPSolver::GLOP_LINEAR_PROGRAMMING) ;
    lp::MPObjective* const objective = solver.MutableObjective() ;
    vector<lp::MPVariable*> track_variables ;
    unordered_map<SimpleTrack, int> track_index ;
    int m = 0 ;
    for (int i = batch_b; i < batch_e; i++) {
        for (auto track = clusters[i].begin(); track != clusters[i].end(); track++) {
            if (tracks.find(*track) != tracks.end()) {
                track_variables.push_back(solver.MakeNumVar(0.0, 1.0, track->get_name())) ;
                track_index[*track] = track_variables.size() - 1 ;
                m += tracks[*track].size() ;
            }
        }
    }
    vector<lp::MPVariable*> error_variables ;
    unordered_map<uint64_t, bool> lp_kmers ;
    string name = clusters[batch_b][0].get_name() ;
    auto track = clusters[batch_b][0] ;
    cout << name << " with " << clusters[batch_b].size() << " tracks and " << m << " kmers." << endl ;
    int n = 0 ;
    vector<ofstream> json_files ;
    for (int i = batch_b; i < batch_e; i++) {
        for (auto track = clusters[i].begin(); track != clusters[i].end(); track++) {
            json_files.emplace_back(ofstream {c->workdir + "/batch_" + track->get_name() + ".json"}) ;
            json_files[n] << "{\n" ;
            n++ ;
        }
    }
    int u = 0 ;
    for (int i = batch_b; i < batch_e; i++) {
        for (auto track = clusters[i].begin(); track != clusters[i].end(); track++) {
            for (auto k = tracks[*track].begin(); k != tracks[*track].end(); k++) {
                if (lp_kmers.find(*k) == lp_kmers.end()) {
                    auto kmer = kmers[*k] ;
                    //cout << decode_kmer(*k) << endl ;
                    // JSON output
                    for (int n = 0; n < json_files.size(); n++) {
                        if (u != 0) {
                            json_files[n] << ",\n" ;
                        }
                        json_files[n] << "\"" << decode_kmer(*k) << "\":" ;
                        json_files[n] << nlohmann::json(kmer).dump(4) ;
                    }
                    u += 1 ;
                    // Adjust GC
                    int gc = 0 ;
                    for (auto locus: kmer.loci) {
                        gc += locus.gc ;
                    }
                    kmer.gc = gc / kmer.loci.size() ;
                    lp_kmers[*k] = true ;
                    int count = kmer.inverse ? kmer.total - kmer.count : kmer.count ;
                    int residue = kmer.inverse ? 0 : kmer.loci.size() - kmer.tracks.size() ;
                    int lp_count = min(count, int(this->coverage * kmer.loci.size())) ;
                    //cout << count << " " << coverage << " " << lp_count << endl ;
                    //cout << kmer.count << " " << coverage << " " << lp_count << endl ;
                    error_variables.push_back(solver.MakeNumVar(-1000, 1000, "e_" + decode_kmer(*k))) ;
                    error_variables.push_back(solver.MakeNumVar(0, 1000, "l_" + decode_kmer(*k))) ;
                    int n = error_variables.size() ;
                    // error absolute value
                    lp::MPConstraint* const abs_1 = solver.MakeRowConstraint(0.0, lp::MPSolver::infinity(), "abs_1_" + decode_kmer(*k));
                    abs_1->SetCoefficient(error_variables[n - 1], 1) ;
                    abs_1->SetCoefficient(error_variables[n - 2], +1.0) ;
                    lp::MPConstraint* const abs_2 = solver.MakeRowConstraint(0.0, lp::MPSolver::infinity(), "abs_2_" + decode_kmer(*k));
                    abs_2->SetCoefficient(error_variables[n - 1], 1) ;
                    abs_2->SetCoefficient(error_variables[n - 2], -1.0) ;
                    objective->SetCoefficient(error_variables[n - 1], 1.0) ;
                    // kmer constraint
                    int rhs = lp_count - residue * coverage ;
                    rhs = max(0, rhs) ;
                    // adjust (1 -l) * C for downward kmers
                    if (!kmer.inverse) {
                        for (auto locus: kmer.loci) {
                            if (locus.type == LOCUS_TYPE_INNER and !locus.trend) {
                                rhs -= int(gc_coverage[locus.gc]) ; 
                            }
                        }
                    }

                    lp::MPConstraint* const ct = solver.MakeRowConstraint(rhs, rhs, "count_" + decode_kmer(*k)) ;
                    for (auto _track = kmer.tracks.begin(); _track != kmer.tracks.end(); _track++) {
                        if (kmer.inverse) {
                            for (auto locus: kmer.junction_loci) {
                                ct->SetCoefficient(track_variables[track_index[_track->first]], this->coverage * kmer.tracks[_track->first]) ;
                            }
                        } else {
                            for (auto locus: kmer.loci) {
                                if (locus.type == LOCUS_TYPE_JUNCTION && locus.position == _track->first.begin) {
                                    ct->SetCoefficient(track_variables[track_index[_track->first]], this->coverage * kmer.tracks[_track->first]) ;
                                }
                                if (locus.type == LOCUS_TYPE_INNER && locus.position >= _track->first.begin - 32 && locus.position <= _track->first.end) {
                                    if (locus.trend) {
                                        ct->SetCoefficient(track_variables[track_index[_track->first]], gc_coverage[locus.gc] * kmer.tracks[_track->first]) ;
                                    } else {
                                        ct->SetCoefficient(track_variables[track_index[_track->first]], -1 * gc_coverage[locus.gc] * kmer.tracks[_track->first]) ;
                                    }
                                }
                            }
                        }
                    }
                    ct->SetCoefficient(error_variables[n - 2], 1.0) ;
                }
            }
        }
    }
    for (int n = 0; n < json_files.size(); n++) {
        json_files[n] << "\n}\n" ;
    }
    objective->SetMinimization() ;
    // Export program
    std::ofstream o ;
    o.open(c->workdir + "/batch_" + name + ".lp") ;
    string program ;
    solver.ExportModelAsLpFormat(false, &program) ;
    o << program ; 
    solver.SetTimeLimit(absl::Seconds(100)) ;
    auto result = solver.Solve() ;
    if (result == lp::MPSolver::OPTIMAL) {
        for (auto track = track_index.begin(); track != track_index.end(); track++) {
            genotyping_tracks[track->first].lp_value = track_variables[track->second]->solution_value() ;
        }
    } else {
        if (result == lp::MPSolver::FEASIBLE) {
            for (auto track = track_index.begin(); track != track_index.end(); track++) {
                genotyping_tracks[track->first].lp_value = track_variables[track->second]->solution_value() ;
            }
        } else {
            cout << "Ignoring " << track_index.size() << " tracks on cluster " << name << endl ;
            for (auto track = track_index.begin(); track != track_index.end(); track++) {
                genotyping_tracks[track->first].lp_value = -1.0 ;
            }
        }
    }
}
#endif

void Genotyper::export_genotypes() {
    cout << "Exporting genotypes.." << endl ;
    auto c = Configuration::getInstance() ;
    std::ofstream o ;
    string name = c->bam[0].substr(c->bam[0].rfind('/'), c->bam[0].length() - c->bam[0].rfind('/')) ;
    o.open(c->cgc ? c->workdir + "/" + name + ".bed" : c->workdir + "/genotypes.bed") ;
    o << "#CHROM\tBEGIN\tEND\tSVTYPE\tLP\tGENOTYPE\n" ;
    for (auto it = genotyping_tracks.begin(); it != genotyping_tracks.end(); it++) {
        auto t = it->first ;
        auto g = it->second ;
        string genotype ;
        if (g.lp_value == -1.0) {
            genotype = "./." ;
        } else {
            if (g.lp_value >= 0.75) {
                genotype = "1/1" ;
            } else if (g.lp_value >= 0.25) {
                genotype = "1/0" ;
            } else {
                genotype = "0/0" ;
            }
        }
        o << get_chromosome_name(t.chrom) << "\t" << t.begin << "\t" << t.end << "\t" << get_svtype(t.svtype) << "\t" << g.lp_value << "\t" << genotype << endl ;
    } 
}
