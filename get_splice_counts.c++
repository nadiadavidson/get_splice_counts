/**
 ** Author: Nadia Davidson, nadia.davidson@mcri.edu.au
 ** Modified: 
 **/ 

#include <iostream>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <sstream>
#include <experimental/string_view>
#include <algorithm>

#include "htslib/sam.h"
#include "htslib/faidx.h"
#include "htslib/kstring.h"
#include "htslib/khash.h"
#include "samtools.h"

#include "BamReader.h"

//#include <gperftools/profiler.h>
using namespace std;
using namespace std::experimental;

static const int FLANK_SIZE=30;
static const bool ALLOW_MISMATCH=false;
static const int MIN_GAP=100; //200000;
static const string END_LABEL=":END";
static const string START_LABEL=":START";
static const char EXON_ID_DELIM=':';
static const int MIN_COUNTS=2;

static int n_first_match=0;
static int n_perfect_match=0;

static BamReader bam_reader;

//class JunctionSeq;
//unordered_map<string_view,string>::iterator find_non_exact_match(string_view & orig_kmer,JunctionSeq& junc_seq);

//complimenting code taken from stackoverflow
char compliment(char& c){
  switch(c){
  case 'A' : return 'T';
  case 'T' : return 'A';
  case 'G' : return 'C';
  case 'C' : return 'G';
  default: return 'N';
  }
}

static class Counts {
  unordered_map< string, int > _counts;
  void print_if_interesting_junction(string name, int & read_support, 
				     unordered_set<string> & black_list,
				     ofstream & ofs_fusions){
    //get positions and genes from the exon junction ids
    stringstream ss(name);    
    string field;
    vector<string> gene_info;
    while(getline(ss,field, EXON_ID_DELIM)) {
      gene_info.push_back(field);
    } //in case the format looks wrong.
    if(gene_info.size()!=8){
      cerr << "Issue with exon sequence IDs." 
	   << "Format should be Gene:Chrom:Start:END/START(+/-)" << endl;
      exit(1);
    }

    //otherwise fill in the gene info
    vector<string> gene{gene_info.at(0),gene_info.at(4)};
    vector<string> chrom{gene_info.at(1),gene_info.at(5)};
    vector<int>pos{atoi(gene_info.at(2).c_str()), 
	atoi(gene_info.at(6).c_str())};
    vector<char> strand{gene_info.at(3).at(gene_info.at(3).size()-2),
	gene_info.at(7).at(gene_info.at(7).size()-2)}; //get the second last character
    
    //now check if the junction looks interesting
    bool different_chrom = chrom[0]!=chrom[1];
    bool non_linear_order = (strand[0]!=strand[1]) | 
      ( strand[0]=='+' & pos[1] < pos[0] ) |
      ( strand[0]=='-' & pos[1] > pos[0] ) ;
    bool distal = (abs(pos[1]-pos[0])>MIN_GAP); // & (gene[0]!=gene[1]);
    bool enough_support = read_support >= MIN_COUNTS;
    stringstream junc_pos_formatted;
    junc_pos_formatted << gene[0] << "\t" << chrom[0] << "\t" << pos[0] << "\t"
		       << gene[1] << "\t" << chrom[1] << "\t" << pos[1] ;
    bool not_in_black_list = black_list.find(junc_pos_formatted.str())==black_list.end();
      
    string event_type ;
    gene[0]==gene[1] ? event_type="BACK_SPLICE" : event_type="FUSION" ;

    if( (different_chrom | non_linear_order | distal ) & enough_support & not_in_black_list){
      ofs_fusions << junc_pos_formatted.str() << "\t" << read_support 
		  << "\t" << bam_reader.get_coverage(chrom[0],pos[0],pos[0]) 
		  << "\t" << bam_reader.get_coverage(chrom[1],pos[1],pos[1])
	   << "\t" << event_type << "\t" << strand[0] << "\t" << strand[1] << endl;
    }
  };
  
public:
  void increment(string& end,string& start){
    string pair = end + EXON_ID_DELIM + start;
    _counts[pair]++;
  };
  void print_table(unordered_set<string> & black_list, string outfile ){
    cerr << "Reporting counts..."<< endl;
    //Sort the count table by counts (highest first)
    //requires conversion to a vector
    vector<pair<string,int>> count_vec(_counts.begin(),_counts.end());
    sort(count_vec.begin(), count_vec.end(), [=](pair<string, int>& a, pair<string, int>& b){
	return a.second > b.second;
      });
    vector< pair<string, int >>::iterator counts_itr=count_vec.begin();
    ofstream ofs_fusions(outfile);
    for(;counts_itr!=count_vec.end(); counts_itr++){
      //if the event is not in the black list check if it's interesting..
      print_if_interesting_junction(counts_itr->first,counts_itr->second,black_list, ofs_fusions);
    }
    ofs_fusions.close();
  };

} counts;

class JunctionSeq { //read the fasta
  unordered_map<string_view,string > junc_seq;
public:
  void read_fasta( unordered_map<string,string> & _seqs, const string type){
    //loop through the sequences and sort into start or end flanking sequence
    //mark any duplicate sequences for later removal
    vector<string_view> to_erase; //list of junction sequences that aren't unique.
    unordered_map<string,string>::iterator seq_itr=_seqs.begin();
    for(; seq_itr!=_seqs.end(); seq_itr++){
      //if more than one junction with this sequence
      //will need to remove later.
      string id=seq_itr->first;
      string_view seq=seq_itr->second;
      bool is_type = id.find(type)!=string::npos;
      bool right_size = seq.size()==FLANK_SIZE;
      if(is_type & right_size){
	//unordered_map<string_view,string>::iterator match =
	//find_non_exact_match(seq,*this);
	//if(match!=junc_seq.end()){
	  if(junc_seq.find(seq)!=junc_seq.end()){
	  to_erase.push_back(seq); //remove edges that differ by only one.
	  //to_erase.push_back(match->first);
	}
	junc_seq[seq]=id;
      }
    }
    if(junc_seq.size()==0){
      cerr << "Found no compatible sequences in exon reference fasta file"<< endl;
      exit(1);
    }

    sort(to_erase.begin(),to_erase.end());
    to_erase.erase(unique(to_erase.begin(),to_erase.end()),to_erase.end());
    //now loop again and remove all the black listed junctions
    cerr << "Removing " << to_erase.size() 
	 << " exon edge sequences for being non-unique" << endl;
    for(int i=0; i<to_erase.size(); i++)
      junc_seq.erase(to_erase.at(i));
    cerr << junc_seq.size() << " edges remaining" << endl;
  };
  inline unordered_map<string_view,string>::iterator find(string_view & key){
    return junc_seq.find(key);
  };
  inline unordered_map<string_view,string>::iterator end(){
    return junc_seq.end();
  };
};

static JunctionSeq junc_seq_start;
static JunctionSeq junc_seq_end;

unordered_map<string_view,string>::iterator 
find_non_exact_match(string_view & orig_kmer,JunctionSeq& junc_seq){
  string kmer(orig_kmer);
  for(int base=0; base < FLANK_SIZE ; base++){
    vector<string> nuc{"A","G","C","T"};
    for(int n=0; n< nuc.size(); n++){
      kmer.replace(base,1,nuc.at(n));
      string_view sv_kmer=kmer;
      unordered_map<string_view,string>::iterator match=junc_seq.find(sv_kmer);
      if(match!=junc_seq.end())
	return match;
    }
  }
  return junc_seq.end();
}

//loop through the sequence to find a match
bool get_match(string & seq){
  if(seq.size()<(2*FLANK_SIZE)) return false;
  unordered_map<string_view,string>::iterator end; //end of exon1
  unordered_map<string_view,string>::iterator start; //joins to start of exon2
  string_view sv_seq = seq;
  string_view kmer1,kmer2;
  //search in the forward direction
  for(int pos=0; pos < (seq.size()-FLANK_SIZE) ; pos++){
    kmer1=sv_seq.substr(pos,FLANK_SIZE);
    end=junc_seq_end.find(kmer1);
    //if a match is found. look for other side of the junction
    if(end!=junc_seq_end.end()){
      n_first_match++;
      kmer2=sv_seq.substr(pos+FLANK_SIZE,FLANK_SIZE);
      start=junc_seq_start.find(kmer2);
      //if other end is found
      if(start!=junc_seq_start.end()){
	n_perfect_match++;
	//cout << "FOUND perfect match" << endl;
	counts.increment(end->second,start->second);
	return true;
      }
      //start not found. Try permutating the bases to account for 1 mismatch
      if(ALLOW_MISMATCH){
	start=find_non_exact_match(kmer2,junc_seq_start);
	if(start!=junc_seq_start.end()){
	  counts.increment(end->second,start->second);
	  return true;
	}
      }
    }
  }
  if(ALLOW_MISMATCH){
    //check again in reverse, permutating the end bases:
    for(int pos=seq.size()-FLANK_SIZE-1; pos >= FLANK_SIZE ; pos--){
      kmer1=sv_seq.substr(pos,FLANK_SIZE);
      start=junc_seq_start.find(kmer1);
      //if a match is found. look for other side of the junction
      if(start!=junc_seq_start.end()){
	kmer2=sv_seq.substr(pos-FLANK_SIZE,FLANK_SIZE);
	end=find_non_exact_match(kmer2,junc_seq_end);
	if(end!=junc_seq_end.end()){
	  counts.increment(end->second,start->second);
	  return true;
	}
      }
    }
  }
  return false;
}

int main(int argc, char *argv[]){
  if(!(argc==5 | argc==6)){
    cerr << "Usage: get_non_linear_region <exon_flanking_seq.fasta> <SNP positions> <output prefix> <in.bam> [black_list]" << endl;
    exit(1);
  }
  string flank_fasta=argv[1];
  string in_filename=argv[4];
  string SNP_pos_file=argv[2];
  string out_prefix=argv[3];
  string SNPs_out_file=out_prefix+".ad";
  string fusions_out_file=out_prefix+".fus";

  unordered_set<string> black_list;
  if(argc==6){ // read the black list
    ifstream black_list_stream;
    black_list_stream.open(argv[5]);
    if(!(black_list_stream.good())){ //check it opens
      cout << "Unable to open file " << argv[5] << endl;
      exit(1);
    }
    string line;
    while(getline (black_list_stream,line)){
      black_list.insert(line);
    }
  }
  
  //Read the fasta reference file
  cerr << "Reading fasta file of junction sequences: " << flank_fasta << endl;
  unordered_map<string,string> seqs;
  ifstream file;
  file.open(flank_fasta);
  if(!(file.good())){ //check it opens
    cout << "Unable to open file " << flank_fasta << endl;
    exit(1);
  } 
  string id="";
  string line;
  while ( getline (file,line) ){
    int start=line.find(">")+1;
    if(start==1){ //if this is the ID line...
        int end=line.find_first_of("\t\n ")-1;
        id=line.substr(start,end);
    } else {
      seqs[id]=seqs[id]+line;
    }
  }
  //pass to function for junction map creation
  junc_seq_start.read_fasta(seqs,START_LABEL);
  junc_seq_end.read_fasta(seqs,END_LABEL);
  cerr << "Done reading fasta" << endl;
  //ProfilerStart("prof.out");

  //Read the bam file (using htslib API)
  bam_reader.setFile(in_filename);  

  int r;
  int i=0;
  int nread_processed=0;
  int f_count=0;
  int r_count=0;
  int nread=0;
  int unmapped=0;

  string seq;
  while((seq=bam_reader.get_next_bad_map_seq(FLANK_SIZE,MIN_GAP))!=""){
    nread_processed++;
    //loop through the sequence to find the first match
    if(get_match(seq)){
      f_count++;
      //also check the reverse compliment if the read is unmapped.
    } else { //if (bam_reader.is_current_read_unaligned()) {
      //unmapped++;
      reverse(seq.begin(),seq.end());
      transform(seq.begin(),seq.end(),seq.begin(),compliment);
      if(get_match(seq)){
	r_count++;
	//	cout << seq << endl;
      }
    }
    //find all matches
  }

  //  ProfilerStop();

  cerr << "Reads Total=" << nread << endl;
  cerr << "Reads Processed=" << nread_processed << endl;
  cerr << "One match=" << n_first_match << endl;
  cerr << "Junction counts=" << f_count << "   " << r_count << endl;
  cerr << "Perfect matches=" << n_perfect_match << endl;
  cerr << "Unmapped=" << unmapped << endl;

  //print out the table of counts
  //bam_reader.setFile(in_filename);
  counts.print_table(black_list,fusions_out_file);

  //Get the SNPs
  file.close();

  /**  cerr << "Finished finding breaks.. getting SNP Allele Depths" << endl;

  file.open(SNP_pos_file);
  if(!(file.good())){ //check it opens
    cout << "Unable to open SNP position file." << endl;
    exit(1);
  }
  string chrom;
  int pos;

  ofstream ofs_SNPs(SNPs_out_file);
  while ( getline (file,line) ){
    stringstream sline(line);
    sline >> chrom;
    sline >> pos ;
    pair<int,int> adepth = bam_reader.get_allele_depth(chrom,pos);
    ofs_SNPs << chrom << "\t" << pos << "\t" ;
    ofs_SNPs << adepth.first << "\t" << adepth.second << endl;
  }
  ofs_SNPs.close(); **/
  // bam_reader.destroy();

}
