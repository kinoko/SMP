/// General Matrix Multiplication
#include <HElib/FHE.h>
#include <HElib/FHEContext.h>
#include <HElib/EncryptedArray.h>
#include <HElib/NumbTh.h>

#include "SMP/DoublePacking.hpp"
#include "SMP/Matrix.hpp"
#include "SMP/Timer.hpp"
#include "SMP/HElib.hpp"
#include "SMP/literal.hpp"
#include "SMP/network/net_io.hpp"

#include <iostream>
#include <numeric>
#include <list>
constexpr int REPEAT = 50;

inline long round_div(long a, long b) {
	return (a + b - 1) / b;
}

void zero(Matrix &mat) {
	for (long i = 0; i < mat.NumRows(); i++)
		for (long j = 0; j < mat.NumCols(); j++)
			mat[i][j] = 0;
}

void randomize(Matrix &mat, long p = 3) {
	for (long i = 0; i < mat.NumRows(); i++)
		for (long j = 0; j < mat.NumCols(); j++)
			mat[i][j] = NTL::RandomBnd(4);
}


void fill_compute(Matrix& mat,
				  long row_blk,
				  long col,
				  const std::vector<long> &inner_prod,
				  const EncryptedArray *ea)
{
	bool is_vec = mat.NumRows() == 1;
	const long l = ea->size();
	assert(inner_prod.size() == l);
	const long row_start = row_blk * l;
	for (long ll = 0; ll < l; ll++) {
		long computed = inner_prod[ll];
		long row = row_start + ll;
		if (row < mat.NumRows()) {
			if (is_vec)
				mat.put(col, row, computed);
			else
				mat.put(row, col, computed);
		} else {
			break;
		}
	}
}

struct ClientBenchmark {
	std::vector<double> pack_times;
	std::vector<double> enc_times;
	std::vector<double> dec_times;
	std::vector<double> unpack_times;
	std::vector<double> total_times;
        int ctx_sent, ctx_rev;
};
ClientBenchmark clt_ben;

struct ServerBenchmark {
	std::vector<double> eval_times;
};
ServerBenchmark srv_ben;

void play_server(std::list<Ctxt> &results,
                 long n1, Matrix const& B,
                 std::vector<std::vector<Ctxt>> const& enc_A_blks,
                 FHEPubKey const& ek,
                 FHEcontext const& context)
{
	NTL::zz_p::init(context.zMStar.getP());
	const EncryptedArray *ea = context.ea;
	const long l = ea->size();
	const long d = ea->getDegree();

	NTL::SetSeed(NTL::to_ZZ(123)); /// use same seed for debugging
	const long MAX_X1 = round_div(n1, l);
	const long MAX_Y1 = round_div(B.NumRows(), d);

	Matrix Bt;
	/// We compute A*B, but we use B tranpose.
	/// This allow us to write one internal::partition()
	/// for row-major case.
	transpose(&Bt, B);
	const long MAX_X2 = round_div(Bt.NumRows(), l);
	const long MAX_Y2 = round_div(Bt.NumCols(), d);
	assert(MAX_Y1 == MAX_Y2);
	NTL::Mat<internal::PackedRows> plain_B_blk; // 2D-array of polynomials
	plain_B_blk.SetDims(MAX_X2, MAX_Y2);
	for (int y = 0; y < MAX_X2; y++) {
		for (int k = 0; k < MAX_Y2; k++) {
			internal::BlockId blk = {y, k};
			plain_B_blk[y][k] = internal::partition(Bt, blk, ea, true);
		}
	}

	/// compute the matrix mulitplication
	double computation{0.};
	do {
		AutoTimer timer(&computation);
		for (long A_blk_idx = 0; A_blk_idx < MAX_X1; A_blk_idx++) {
			for (long col_B = 0; col_B < B.NumCols(); col_B++) {
				long B_blk_idx = col_B / l;
				long offset = col_B % l;
				assert(B_blk_idx <= plain_B_blk.NumRows());
				Ctxt summation(ek);
				for (long prtn = 0; prtn < MAX_Y1; prtn++) {
					Ctxt enc_blk(enc_A_blks.at(A_blk_idx).at(prtn));
					NTL::ZZX plain_blk;
					NTL::conv(plain_blk, plain_B_blk[B_blk_idx][prtn].polys.at(offset));
					enc_blk.multByConstant(plain_blk);
					summation += enc_blk;
				}
				summation.modDownToLevel(1);
				results.push_back(summation);
			}
		}
	} while (0);
	srv_ben.eval_times.push_back(computation);
}

void play_client(FHESecKey &sk,
		 FHEcontext &context,
		 const long n1,
		 const long n2,
		 const long n3) 
{
	//* Convert to evalution key.
	//* This function is not provided by the origin HElib. Checkout our fork.
    sk.convertToSymmetric();
	FHEPubKey ek(sk);
	const EncryptedArray *ea = context.ea;
	const long l = ea->size();
	const long d = ea->getDegree();

	NTL::SetSeed(NTL::to_ZZ(123));
	Matrix A, B, ground_truth; // clients hold A, server holds B
	A.SetDims(n1, n2);
	B.SetDims(n2, n3);
	randomize(A, ek.getPtxtSpace());
	randomize(B, ek.getPtxtSpace());
	ground_truth = mul(A, B);
	/// print grouth truth for debugging
	const long MAX_X1 = round_div(A.NumRows(), l);
	const long MAX_Y1 = round_div(A.NumCols(), d);
	const long MAX_X2 = round_div(B.NumCols(), l);

	std::vector<std::vector<Ctxt>> uploading;
	uploading.resize(MAX_X1, std::vector<Ctxt>(MAX_Y1, Ctxt(sk)));
	double enc_time = 0.;
	double pack_time = 0.;
	/// encrypt matrix
	NTL::ZZX packed_poly;
	for (int x = 0; x < MAX_X1; x++) {
		for (int k = 0; k < MAX_Y1; k++) {
			internal::BlockId blk = {x, k};
			double one_pack_time = 0., one_enc_time = 0.;
			auto block = internal::partition(A, blk, ea, false);
			{/// packing
				AutoTimer timer(&one_pack_time);
				rawEncode(packed_poly, block.polys, context);
			}
			{/// encryption
				AutoTimer timer(&one_enc_time);
				sk.Encrypt(uploading[x][k], packed_poly);
			}
			pack_time += one_pack_time;
			enc_time += one_enc_time;
		}
	}
	clt_ben.pack_times.push_back(pack_time);
	clt_ben.enc_times.push_back(enc_time);
	clt_ben.ctx_sent = MAX_X1 * MAX_Y1;

	std::vector<GMMPrecompTable> tbls = precompute_gmm_tables(context);
	/// waiting results
	long rows_of_A = A.NumRows();
	long rows_of_Bt = B.NumCols(); // Bt::Rows = B::Cols
        std::list<Ctxt> ret_ctxs;
        play_server(ret_ctxs, n1, B, uploading, sk, context);
        clt_ben.ctx_rev = ret_ctxs.size();
	/// decrypt
	Matrix computed;
	computed.SetDims(A.NumRows(), B.NumCols());
	zero(computed);
	int x = 0;
	int y = 0;
	std::vector<long> slots;
    NTL::Vec<long> decrypted;
	double decrypt_time = 0.;
	double unpack_time = 0.;
	long ctx_idx = 0;
	bool dec_pass = true;
	for (const auto &ctx : ret_ctxs) {
		double one_dec_time = 0., one_unpack_time = 0.;
		do {
			AutoTimer timer(&one_dec_time);
			dec_pass &= ctx.isCorrect();
			faster_decrypt(decrypted, sk, ctx);
			//sk.Decrypt(decrypted, ctx);
		} while(0);
		do {
			AutoTimer timer(&one_unpack_time);
			extract_inner_products(slots, decrypted, tbls, context);
		} while(0);
		decrypt_time += one_dec_time;
		unpack_time += one_unpack_time;

		long row_blk = ctx_idx / B.NumCols();
		long column = ctx_idx % B.NumCols();
		ctx_idx += 1;
		fill_compute(computed, row_blk, column, slots, ea);
	}
	clt_ben.dec_times.push_back(decrypt_time);
	clt_ben.unpack_times.push_back(unpack_time);
	if (!::is_same(ground_truth, computed, NTL::zz_p::modulus()))
		std::cerr << "The computation seems wrong " << std::endl;
	//if (!dec_pass)
        //	std::cerr << "Decryption might fail" << std::endl;
}

int run(long n1, long n2, long n3) {
	const long m = 8192;
	const long p = 70913;
	const long r = 1;
	const long L = 2;
	NTL::zz_p::init(p);
	FHEcontext context(m, p, r);
	context.bitsPerLevel = 60;
	buildModChain(context, L);
	FHESecKey sk(context);
	sk.GenSecKey(64);
	for (long t = 0; t < REPEAT; t++) {
		double all_time = 0.;
		{
			AutoTimer time(&all_time);
			play_client(sk, context, n1, n2, n3);
		}
		clt_ben.total_times.push_back(all_time);
	}
	return 1;
}

int main(int argc, char *argv[]) {
	ArgMapping argmap;
	long n1 = 128;
	long n2 = 128;
	long n3 = 128;
	argmap.arg("N", n1, "n1");
	argmap.arg("M", n2, "n2");
	argmap.arg("D", n3, "n3");
	argmap.parse(argc, argv);

	run(n1, n2, n3);

	auto time = mean_std(clt_ben.pack_times);
	printf("%.3f %.3f ", time.first, time.second);

	time = mean_std(clt_ben.enc_times);
	printf("%.3f %.3f ", time.first, time.second);

	time = mean_std(clt_ben.dec_times);
	printf("%.3f %.3f ", time.first, time.second);

	time = mean_std(clt_ben.unpack_times);
	printf("%.3f %.3f ", time.first, time.second);

	time = mean_std(clt_ben.total_times);
	printf("%.3f %.3f ", time.first, time.second);

	time = mean_std(srv_ben.eval_times);
	printf("%.3f %.3f ", time.first, time.second);
        printf("%d %d\n", clt_ben.ctx_sent, clt_ben.ctx_rev);
	return 0;
}
