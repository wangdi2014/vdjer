#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "htslib/sam.h"
#include "htslib/faidx.h"
#include "htslib/kstring.h"
#include "htslib/khash.h"
#include "samtools.h"
#include "hash_utils.h"

#include <sparsehash/dense_hash_set>

using google::dense_hash_set;

//extern int kmer_size;
int kmer_size=25;
int EXTRACT_KMER_SIZE = 15;

struct bam_info {
	samFile *in;
	hts_idx_t *idx;
	bam_hdr_t *header;
};

int bam_open(char* bam_file, bam_info* bam) {
	memset(bam, 0, sizeof(bam_info));

	bam->in = sam_open(bam_file, "r");
	if (bam->in == NULL) return -1;

	if ((bam->header = sam_hdr_read(bam->in)) == 0) return -1;

	bam->idx = sam_index_load(bam->in,  bam_file);
	if (bam->idx == NULL) return -1;

	return 0;
}

void bam_close(bam_info* bam_info) {
	bam_hdr_destroy(bam_info->header);
	sam_close(bam_info->in);
}

void bam_get_seq_str(bam1_t *b, char* seq) {
	uint8_t *b_seq = bam_get_seq(b);
	for (int i=0; i<b->core.l_qseq; i++) {
	  seq[i] = "=ACMGRSVTWYHKDBN"[bam_seqi(b_seq, i)];
	}
	seq[b->core.l_qseq] = '\0';
}

void bam_get_qual_str(bam1_t *b, char* quals) {
	uint8_t *b_quals = bam_get_qual(b);
	for (int i=0; i<b->core.l_qseq; i++) {
		quals[i] = b_quals[i] + 33;
	}
	quals[b->core.l_qseq] = '\0';
}

void test_interval(char* bam_file, char* interval) {
    bam_info bam;
    bam_open(bam_file, &bam);

    hts_itr_t *iter = sam_itr_querys(bam.idx, bam.header, interval);
    bam1_t *b = bam_init1();

	printf("Iterate\n");
	while ( sam_itr_next(bam.in, iter, b) >= 0) {
	  char* qname = bam_get_qname(b);
	  kstring_t str = { 0, 0, NULL };
	  sam_format1(bam.header, b, &str);

	  char seq[256];
	  bam_get_seq_str(b, seq);

	  char quals[256];
	  bam_get_qual_str(b, quals);

	  printf("seq: [%s]\tquals: [%s]\n", seq, quals);
	  printf("str: [%s]\n", str.s);
	}

	hts_itr_destroy(iter);
	bam_destroy1(b);
	bam_close(&bam);
}

void test_unmapped(char* bam_file) {
    bam_info bam;
    bam_open(bam_file, &bam);

    bam1_t *b = bam_init1();
	int r;
	while ((r = sam_read1(bam.in, bam.header, b)) >= 0) {
		if (b->core.flag & 4) {
			char* qname = bam_get_qname(b);
			char seq[256];
			bam_get_seq_str(b, seq);

			char quals[256];
			bam_get_qual_str(b, quals);

			printf("Unmapped\tq: [%s]\tseq: [%s]\tquals: [%s]\n", qname, seq, quals);
		}
	}
}

//TODO: Move the following from quick_map2.c
char complement(char ch) {
	switch(ch) {
		case 'A':
			return 'T';
		case 'T':
			return 'A';
		case 'C':
			return 'G';
		case 'G':
			return 'C';
		default:
			return ch;
	}
}

int rc(char* input, char* output) {
	int out_idx = 0;
	for (int i=strlen(input)-1; i >=0; i--) {
		output[out_idx++] = complement(input[i]);
	}
	output[strlen(input)] = '\0';
}

int reverse(char* input, char* output) {
	int out_idx = 0;
	for (int i=strlen(input)-1; i >=0; i--) {
		output[out_idx++] = input[i];
	}
	output[strlen(input)] = '\0';
}


dense_hash_set<const char*, my_hash, eqstr> extract_vdj_kmers;
char* extract_vdj_kmers_buf = (char*) calloc(1000000, sizeof(char));  // 1MB should be more than enough.
char* extract_vdj_kmers_buf_ptr = extract_vdj_kmers_buf;

char contains_str(dense_hash_set<const char*, vjf_hash, vjf_eqstr>& str_set, char* str) {
	dense_hash_set<const char*, vjf_hash, vjf_eqstr>::const_iterator it = str_set.find(str);
	return it != str_set.end();
}

char* get_str(dense_hash_set<const char*, vjf_hash, vjf_eqstr>& str_set, char* str) {
	dense_hash_set<const char*, vjf_hash, vjf_eqstr>::const_iterator it = str_set.find(str);
	return (char*) *it;
}

char contains(dense_hash_set<const char*, my_hash, eqstr>& str_set, char* str) {
	dense_hash_set<const char*, my_hash, eqstr>::const_iterator it = str_set.find(str);
	return it != str_set.end();
}

char is_kmer_in_set(char* kmer) {
	return contains(extract_vdj_kmers, kmer);
}

void add_kmer(char* kmer) {
	if (!is_kmer_in_set(kmer)) {
		strncpy(extract_vdj_kmers_buf_ptr, kmer, EXTRACT_KMER_SIZE);
		extract_vdj_kmers_buf_ptr[EXTRACT_KMER_SIZE] = '\0';
		extract_vdj_kmers.insert(extract_vdj_kmers_buf_ptr);
		extract_vdj_kmers_buf_ptr += EXTRACT_KMER_SIZE+1;
	}
}

void load_kmers(char* vdj_fasta) {
	extract_vdj_kmers.set_empty_key(NULL);

	// Load kmers

	FILE* vdj = fopen(vdj_fasta, "r");
	if (vdj == NULL) {
		printf("Could not open file: [%s]", vdj_fasta);
	}

	char buf[1024];
	char rc_buf[1024];

	while (fgets (buf, sizeof(buf), vdj)) {
		if (buf[0] != '>') {
			buf[strlen(buf)-1] = '\0'; // Remove newline
			rc(buf, rc_buf);

			for (int i=0; i<strlen(buf) - EXTRACT_KMER_SIZE; i+= 1) {
				add_kmer(buf+i);
				add_kmer(rc_buf+i);
			}
		}
	}
}

void add_to_buffer(bam1_t *b, char*& buf_ptr, int read_len) {

	char seq[256];
	char quals[256];
	char rc_seq[256];
	char r_quals[256];

	bam_get_seq_str(b, seq);
	bam_get_qual_str(b, quals);

	buf_ptr[0] = '0';
	buf_ptr += 1;
	strncpy(buf_ptr, seq, read_len);
	buf_ptr += read_len;
	strncpy(buf_ptr, quals, read_len);
	buf_ptr += read_len;

	buf_ptr[0] = '0';
	buf_ptr += 1;
	rc(seq, rc_seq);
	strncpy(buf_ptr, rc_seq, read_len);
	buf_ptr += read_len;

	reverse(quals, r_quals);
	strncpy(buf_ptr, r_quals, read_len);
	buf_ptr += read_len;
}

void extract(char* bam_file, char* vdj_fasta, char* v_region, char* c_region,
		char*& primary_buf, char*& secondary_buf) {

	int orig_kmer_size =  kmer_size;
	kmer_size = EXTRACT_KMER_SIZE;

	dense_hash_set<const char*, vjf_hash, vjf_eqstr> primary_reads;
	primary_reads.set_empty_key(NULL);

	dense_hash_set<const char*, vjf_hash, vjf_eqstr> secondary_reads;
	secondary_reads.set_empty_key(NULL);

	load_kmers(vdj_fasta);

	// TODO: Max buffer size??
	char* read_name_buf = (char*) calloc(100000000, sizeof(char));
	char* read_name_buf_ptr = read_name_buf;
    bam_info bam;
    bam_open(bam_file, &bam);
    bam1_t *b = bam_init1();

    // Cache variable read names
    hts_itr_t *iter = sam_itr_querys(bam.idx, bam.header, v_region);

	while ( sam_itr_next(bam.in, iter, b) >= 0) {
		char* qname = bam_get_qname(b);

		if (!contains_str(primary_reads, qname)) {
			strncpy(read_name_buf_ptr, qname, strlen(qname));
			primary_reads.insert(read_name_buf_ptr);
			read_name_buf_ptr += strlen(qname)+1;
		}
	}

	hts_itr_destroy(iter);

	// Cache constant read names
	hts_itr_t *iter2 = sam_itr_querys(bam.idx, bam.header, c_region);

	while ( sam_itr_next(bam.in, iter2, b) >= 0) {
		char* qname = bam_get_qname(b);

		if (!contains_str(secondary_reads, qname)) {
			strncpy(read_name_buf_ptr, qname, strlen(qname));
			secondary_reads.insert(read_name_buf_ptr);
			read_name_buf_ptr += strlen(qname)+1;
		}
	}

	hts_itr_destroy(iter2);
	int read_len = 0;

	// Process unmapped reads
	int r;
	char seq[256];
	while ((r = sam_read1(bam.in, bam.header, b)) >= 0) {
		if (b->core.flag & 4) {
			char* qname = bam_get_qname(b);
			bam_get_seq_str(b, seq);

			if (read_len == 0) {
				read_len = b->core.l_qseq;
			}

			for (int i=0; i<strlen(seq)-EXTRACT_KMER_SIZE; i++) {
				if (is_kmer_in_set(seq+i)) {
					if (!contains_str(primary_reads, qname)) {
						strncpy(read_name_buf_ptr, qname, strlen(qname));
						primary_reads.insert(read_name_buf_ptr);
						read_name_buf_ptr += strlen(qname)+1;
					}
				} else if (!contains_str(secondary_reads, qname)) {
					strncpy(read_name_buf_ptr, qname, strlen(qname));
					secondary_reads.insert(read_name_buf_ptr);
					read_name_buf_ptr += strlen(qname)+1;
				}
			}
		}
	}

	bam_destroy1(b);
	bam_close(&bam);

	// Allocate room for strand flag * 2, seq * 2, quals * 2 (Forward / Rev complement), pair * 2 (1st in pair, 2nd in pair)
	primary_buf = (char*) calloc(primary_reads.size() * (read_len*8 + 4) + 1, sizeof(char));
	char* primary_buf_ptr = primary_buf;
	secondary_buf = (char*) calloc(secondary_reads.size() * (read_len*8 + 4) + 1, sizeof(char));
	char* secondary_buf_ptr = secondary_buf;

	dense_hash_set<const char*, vjf_hash, vjf_eqstr> primary_output1;
	dense_hash_set<const char*, vjf_hash, vjf_eqstr> primary_output2;

	dense_hash_set<const char*, vjf_hash, vjf_eqstr> secondary_output1;
	dense_hash_set<const char*, vjf_hash, vjf_eqstr> secondary_output2;

	primary_output1.set_empty_key(NULL);
	primary_output2.set_empty_key(NULL);
	secondary_output1.set_empty_key(NULL);
	secondary_output2.set_empty_key(NULL);

	char quals[256];
	char rc_seq[256];
	char rc_quals[256];

	// Reinitialize bam file and start over from beginning...
    bam_open(bam_file, &bam);
    b = bam_init1();

	while ((r = sam_read1(bam.in, bam.header, b)) >= 0) {
		if (!(b->core.flag & 0x900)) {
			char* qname = bam_get_qname(b);
			if (contains_str(primary_reads, qname)) {
				if ((b->core.flag & 0x40)  && !contains_str(primary_output1, qname)) {
					printf("po1: %s\n", qname);
					add_to_buffer(b, primary_buf_ptr, read_len);
					char* qname_str = get_str(primary_reads, qname);
					primary_output1.insert(qname_str);
				} else if ((b->core.flag & 0x80)  && !contains_str(primary_output2, qname)) {
					add_to_buffer(b, primary_buf_ptr, read_len);
					char* qname_str = get_str(primary_reads, qname);
					primary_output2.insert(qname_str);
				}
			} else if (contains_str(secondary_reads, qname)) {
				if ((b->core.flag & 0x40)  && !contains_str(secondary_output1, qname)) {
					add_to_buffer(b, secondary_buf_ptr, read_len);
					char* qname_str = get_str(secondary_reads, qname);
					secondary_output1.insert(qname_str);
				} else if ((b->core.flag & 0x80)  && !contains_str(secondary_output2, qname)) {
					add_to_buffer(b, secondary_buf_ptr, read_len);
					char* qname_str = get_str(secondary_reads, qname);
					secondary_output2.insert(qname_str);
				}
			}
		}
	}

//	for (dense_hash_set<const char*, vjf_hash, vjf_eqstr>::iterator it=primary_reads.begin(); it!=primary_reads.end(); it++) {
//		printf("qname1: %s\n", *it);
//	}
//
//	for (dense_hash_set<const char*, vjf_hash, vjf_eqstr>::iterator it=secondary_reads.begin(); it!=secondary_reads.end(); it++) {
//		printf("qname2: %s\n", *it);
//	}

	bam_destroy1(b);
	bam_close(&bam);

	kmer_size = orig_kmer_size;

//	printf("primary_buf: %s\n", primary_buf);

	free(extract_vdj_kmers_buf);
	free(read_name_buf);

//	for (dense_hash_set<const char*, vjf_hash, vjf_eqstr>::iterator it=primary_reads.begin(); it!=primary_reads.end(); it++) {
//		free((void*) *it);
//	}
}


int main(int argc,char** argv)
{
//	if(argc!=3) return -1;
//    char* bam_file = argv[1];
//    char* interval = argv[2];
//
//    test_interval(bam_file, interval);
//
//    test_unmapped(bam_file);
//
//	return 0;

	char* bam_file = argv[1];
	char* vdj_fasta = argv[2];
	char* v_region = argv[3];
	char* c_region = argv[4];

	char* p1;
	char* p2;

	extract(bam_file, vdj_fasta, v_region, c_region, p1, p2);

	printf("strlen(p1): [%d]\n", strlen(p1));
	printf("strlen(p2): [%d]\n", strlen(p2));
}


