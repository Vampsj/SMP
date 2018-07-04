/// General Matrix Multiplication
#include <HElib/FHE.h>
#include <HElib/FHEContext.h>
#include <HElib/EncryptedArray.h>
#include <HElib/NumbTh.h>

#include "SMP/Matrix.hpp"
#include "SMP/Timer.hpp"
#include "SMP/HElib.hpp"
#include "SMP/literal.hpp"
#include "SMP/network/net_io.hpp"
#include "SMP/SMPServer.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <iostream>
#include <numeric>
#include <list>
using boost::asio::ip::tcp;
constexpr int REPEAT = 1;
std::atomic<int> global_counter(0);

inline long round_div(long a, long b) {
    return (a + b - 1) / b;
}

// both parties set a matrix C0, C1 to a zero matrix
void zero(Matrix &mat) {
    for (long i = 0; i < mat.NumRows(); i++)
        for (long j = 0; j < mat.NumCols(); j++)
            mat[i][j] = 0;
}

// set a matrix with random numbers
void randomize(Matrix &mat, long p = 3) {
    for (long i = 0; i < mat.NumRows(); i++)
        for (long j = 0; j < mat.NumCols(); j++)
            mat[i][j] = NTL::RandomBnd(p);
}

// Fill a computed inner product into a matrix
void fill_compute(Matrix& mat,
				  long row_blk,//row block ?
				  long col,
                  const std::vector<long> &inner_prod,
                  const EncryptedArray *ea)
{
    const long l = ea->size();
    assert(inner_prod.size() == l);
	const bool is_vec = mat.NumRows() == 1;
	const long row_start = is_vec ? 0 : row_blk * l;
	const long col_start = is_vec ? row_blk * l : col;

    for (long ll = 0; ll < l; ll++) {
        long computed = inner_prod[ll];
		if (!is_vec) {
			long row = row_start + ll;
			if (row < mat.NumRows())
				mat.put(row, col, computed);
			else
				break;
		} else {
			long col = col_start + ll;
			if (col < mat.NumCols())
				mat.put(0, col, computed);
			else
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
    int ctx_sent, ctx_recv;
};
ClientBenchmark clt_ben;

struct ServerBenchmark {
    std::vector<double> eval_times;
};
ServerBenchmark srv_ben;

void play_client(tcp::iostream &conn,
                 FHESecKey &sk,
                 FHEcontext &context,
                 const long n1,
                 const long n2,
                 const long n3) {
	//* Convert to evalution key.
	//* This function is not provided by the origin HElib. Checkout our fork.
    sk.convertToSymmetric();
    FHEPubKey ek(sk);
    conn << ek;
    const EncryptedArray *ea = context.ea;
    const long l = ea->size();    // l messages/slots -> l factor polynomials Fk
    const long d = ea->getDegree(); // degree of factor polynomials Fk is d
    // from l and d, we can get m = d * l
    // X^m + 1 = \prod_{k=0}^{l-1} F_k

    // gound_truth = A * B
    Matrix A, B, ground_truth;
    A.SetDims(n1, n2);
    B.SetDims(n2, n3);
    NTL::SetSeed(NTL::to_ZZ(123));
    randomize(A, ek.getPtxtSpace());
    randomize(B, ek.getPtxtSpace());
    ground_truth = mul(A, B);
    /// print grouth truth for debugging
    const long MAX_X1 = round_div(A.NumRows(), l);
    const long MAX_Y1 = round_div(A.NumCols(), d);
    const long MAX_X2 = round_div(B.NumCols(), l);

    // vector of a vector containing ctxts
    std::vector<std::vector<Ctxt>> uploading;
    // <v1,v2,...,v_{MAX_X1}>,
    // where vi = <ctxt(sk), ctxt(sk), ..., ctxt(sk)>
    // and vi.size = MAX_Y1
    uploading.resize(MAX_X1, std::vector<Ctxt>(MAX_Y1, Ctxt(sk)));
	double enc_time = 0.;
    double pack_time = 0.;
	/// encrypt matrix
	NTL::ZZX packed_poly;
	for (int x = 0; x < MAX_X1; x++) {
		for (int k = 0; k < MAX_Y1; k++) {
			internal::BlockId blk = {x, k};
			double one_pack_time = 0.;
			double one_enc_time = 0.;
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

    /// send ciphertexts of matrix
    for (auto const& row : uploading) {
        for (auto const& ctx : row) {
            conn << ctx;
            //_ctx_sent++;
        }
    }
	conn.flush();
    clt_ben.ctx_sent = MAX_X1 * MAX_Y1;
	/// we convert DoubleCRT to poly form when send ciphertexts through, and thus, we
	/// count this cost as a part of encryption.
	clt_ben.enc_times.back() += getTimerByName("TO_POLY_OUTPUT")->getTime() * 1000.;

    std::vector<GMMPrecompTable> tbls = precompute_gmm_tables(context);
    /// waiting results
    long rows_of_A = A.NumRows();
    long rows_of_Bt = B.NumCols(); // Bt::Rows = B::Cols
	int64_t ctx_cnt = 0;
	conn >> ctx_cnt;
    clt_ben.ctx_recv = ctx_cnt;
    std::vector<Ctxt> ret_ctxs(ctx_cnt, Ctxt(ek));
	for (size_t k = 0; k < ctx_cnt; k++) {
		conn >> ret_ctxs.at(k);
        //_ctx_received++;
    }
    double eval_time = 0.;
    conn >> eval_time;
    srv_ben.eval_times.push_back(eval_time);
    /// decrypt
    Matrix computed;
    computed.SetDims(A.NumRows(), B.NumCols());
    zero(computed);
    int x = 0;
    int y = 0;
    std::vector<long> slots;
    std::vector<NTL::zz_pX> _slots;
    //NTL::Vec<long> decrypted;
	NTL::ZZX decrypted;
	double decrypt_time = 0.;
    double unpack_time = 0.;
	long ctx_idx = 0;
	bool dec_pass = true;
	for (const auto &ctx : ret_ctxs) {
		double one_dec_time = 0.;
		double one_unpack_time = 0.;
		do {
			AutoTimer timer(&one_dec_time);
			dec_pass &= ctx.isCorrect();
			//faster_decrypt(decrypted, sk, ctx);
			sk.Decrypt(decrypted, ctx);
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
	/// we convert poly to DoubleCRT when receiving ciphertexts.
	decrypt_time += getTimerByName("FROM_POLY_OUTPUT")->getTime() * 1000.;
    clt_ben.dec_times.push_back(decrypt_time);
    clt_ben.unpack_times.push_back(unpack_time);
	if (!::is_same(ground_truth, computed, NTL::zz_p::modulus()))
		std::cerr << "The computation seems wrong " << std::endl;
	if (!dec_pass)
		std::cerr << "Decryption might fail" << std::endl;
    global_counter++;
}

int run_client(std::string const& addr, long port,
               long n1, long n2, long n3) {
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
    auto start_time_stamp = std::clock();
    auto last_time_stamp = std::clock();
    int done = 0;
    while (true) {
        tcp::iostream conn(addr, std::to_string(port));
        if (!conn) {
            std::cerr << "Can not connect to server!" << std::endl;
            return -1;
        }

        /// send FHEcontext obj
        double all_time = 0.;
        do {
            send_context(conn, context);
            AutoTimer time(&all_time);
            // send the evaluation key
            play_client(conn, sk, context, n1, n2, n3);
        } while(0);
        clt_ben.total_times.push_back(all_time);
        conn.close();
		resetAllTimers(); // reset timers in HElib
		break;
        ++done;
        auto now_time = std::clock();
        if (now_time - last_time_stamp >= 60 * CLOCKS_PER_SEC) {
            std::cout << done << "\n";
            last_time_stamp = now_time;
        }

        if (now_time - start_time_stamp >= 3600 * CLOCKS_PER_SEC) {
            /// one hour
            break;
        }
    }
    std::cout << "one hour finished: " << done << "\n";
    return 0;
}

int run_client_cnn(std::string const& addr, long port) {
    const long m = 8192;
    const long p = 70913;
    const long r = 1;
    const long L = 2;
    long a1 = 1;
    long a2 = 744;
    long a3 = 6138;
    long a4 = 128;
    long a5 = 12;
    NTL::zz_p::init(p);
    FHEcontext context(m, p, r);
    context.bitsPerLevel = 60;
    buildModChain(context, L);
    FHESecKey sk(context);
    sk.GenSecKey(64);
    auto start_time_stamp = std::clock();
    auto last_time_stamp = std::clock();
    int done = 0;
    while (true) {
        tcp::iostream conn(addr, std::to_string(port));
        if (!conn) {
            std::cerr << "Can not connect to server!" << std::endl;
            return -1;
        }
        
        /// send FHEcontext obj
        double all_time = 0.;
        do {
            send_context(conn, context);
            AutoTimer time(&all_time);
            // send the evaluation key
            auto start_time_stamp = std::clock();
            play_client(conn,sk,context,a1,a2,a1);
            play_client(conn,sk,context,a3,a3,a4);
            play_client(conn,sk,context,a4,a4,a4);
            play_client(conn,sk,context,a4,a4,a5);
            auto last_time_stamp = std::clock();
            std::cout << "Total time: " << last_time_stamp - start_time_stamp << "\n";
        } while(0);
        clt_ben.total_times.push_back(all_time);
        conn.close();
        resetAllTimers(); // reset timers in HElib
        break;
        ++done;
        auto now_time = std::clock();
        if (now_time - last_time_stamp >= 60 * CLOCKS_PER_SEC) {
            std::cout << done << "\n";
            last_time_stamp = now_time;
        }
        
        if (now_time - start_time_stamp >= 3600 * CLOCKS_PER_SEC) {
            /// one hour
            break;
        }
    }
    std::cout << "one hour finished: " << done << "\n";
    return 0;
}



int run_server(long port, long n1, long n2, long n3) {
    boost::asio::io_service ios;
    tcp::endpoint endpoint(tcp::v4(), port);
    tcp::acceptor acceptor(ios, endpoint);
    for (long run = 0; run < REPEAT; run++) {
        tcp::iostream conn;
        boost::system::error_code err;
        acceptor.accept(*conn.rdbuf(), err);

        if (!err) {
			SMPServer server;
			server.run(conn, n1, n2, n3);
			resetAllTimers(); // reset timers in HElib
        }
    }
	SMPServer::print_statistics();
    return 0;
}

int run_server_cnn(long port) {
    boost::asio::io_service ios;
    tcp::endpoint endpoint(tcp::v4(), port);
    tcp::acceptor acceptor(ios, endpoint);
    long a1 = 1;
    long a2 = 744;
    long a3 = 6138;
    long a4 = 128;
    long a5 = 12;
    for (long run = 0; run < REPEAT; run++) {
        tcp::iostream conn;
        boost::system::error_code err;
        acceptor.accept(*conn.rdbuf(), err);
        
        if (!err) {
            auto start_time_stamp = std::clock();
            SMPServer server;
            server.run(conn, a1,a2,a1);
            server.run(conn, a3,a3,a4);
            server.run(conn, a4,a4,a4);
            server.run(conn, a4,a4,a5);
            auto last_time_stamp = std::clock();
            std::cout << "Total time: " << last_time_stamp - start_time_stamp << "\n";
            resetAllTimers(); // reset timers in HElib
        }
    }
    SMPServer::print_statistics();
    return 0;
}

int main(int argc, char *argv[]) {
    ArgMapping argmap;
    long role = -1;
    long n1 = 8;
    long n2 = 8;
    long n3 = 8;
    std::string addr = "127.0.0.1";
	long port = 12345;
    argmap.arg("N", n1, "n1");
    argmap.arg("M", n2, "n2");
    argmap.arg("D", n3, "n3");
    argmap.arg("R", role, "role. 0 for server and 1 for client");
	argmap.arg("a", addr, "server address");
	argmap.arg("p", port, "port");
    argmap.parse(argc, argv);
    if (role == 0) {
        //run_server(addr, port, n1, n2, n3);
        run_server_cnn(port);
    } else if (role == 1) {
        //run_client(addr, port, n1, n2, n3);
        run_client_cnn(addr,port);
    } else {
		argmap.usage("General Matrix Multiplication for |N*M| * |M*D|");
		return -1;
	}
}
