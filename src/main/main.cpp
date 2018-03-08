#include <future>
#include <iomanip>
#include <math.h>

#include <boost/program_options.hpp>
#include <boost/optional.hpp>
#include <rapidjson/document.h>
#include <gperftools/profiler.h>

#include "input-output/input/command_line_helper.h"
#include "input-output/input/json-parsing/json_helper.h"
#include "input-output/input/json-parsing/rules_parser.h"
#include "input-output/output/log_helper.h"
#include "solver/solver.h"

using namespace rapidjson;

using namespace std;
using namespace boost;

namespace po = boost::program_options;

const optional<sol_rules> gen_rules(command_line_helper&);
void solve_random_game(int, const sol_rules&, bool, bool);
void solve_input_files(vector<string>, const sol_rules&, bool, bool);
void solve_game(const game_state&, bool, bool);
void calculate_solvability_percentage(const sol_rules&);
double sol_lower_bound(int, int, int);
double sol_upper_bound(int, int, int);
double agrestiCoull(int, int, double, bool);

int main(int argc, const char* argv[]) {

    // Parses the command-line options
    command_line_helper clh;
    if (!clh.parse(argc, argv)) {
        return EXIT_FAILURE;
    }

    // Generates the rules of the solitaire from the game type
    const optional<sol_rules> rules = gen_rules(clh);
    if (!rules) return EXIT_FAILURE;

    // If the user has asked for a solvability percentage, calculates it
    if (clh.get_solvability()) {
        calculate_solvability_percentage(*rules);
    }
    // If a random deal seed has been supplied, solves it
    else if (clh.get_random_deal() != -1) {
        solve_random_game(clh.get_random_deal(), *rules, clh.get_short_sols(),
                          clh.get_classify());
    }
    // Otherwise there are supplied input files which should be solved
    else {
        const vector<string> input_files = clh.get_input_files();

        // If there are no input files, solve a random deal based on the
        // supplied seed
        assert (!input_files.empty());
        solve_input_files(input_files, *rules, clh.get_short_sols(),
                          clh.get_classify());
    }

    return EXIT_SUCCESS;
}

const optional<sol_rules> gen_rules(command_line_helper& clh) {
    try {
        if (!clh.get_solitaire_type().empty()) {
            return rules_parser::from_preset(clh.get_solitaire_type());
        } else {
            return rules_parser::from_file(clh.get_rules_file());
        }
    } catch (const runtime_error& error) {
        string errmsg = "Error in rules generation: ";
        errmsg += error.what();
        LOG_ERROR(errmsg);
        return none;
    }
}

void solve_random_game(int seed, const sol_rules& rules, bool short_sol,
                       bool classify) {
    LOG_INFO ("Attempting to solve with seed: " << seed << "...");
    game_state gs(rules, seed);
    solve_game(gs, short_sol, classify);
}

void solve_input_files(const vector<string> input_files, const sol_rules& rules,
                       bool short_sol, bool classify) {
    for (const string& input_file : input_files) {
        try {
            // Reads in the input file to a json doc
            const Document in_doc = json_helper::get_file_json(input_file);

            // Attempts to create a game state object from the json
            game_state gs(rules, in_doc);

            LOG_INFO ("Attempting to solve " << input_file << "...");
            solve_game(gs, short_sol, classify);

        } catch (const runtime_error& error) {
            string errmsg = "Error parsing deal file: ";
            errmsg += error.what();
            LOG_ERROR(errmsg);
        }
    }
}

void solve_game(const game_state& gs, bool short_sol, bool classify) {
    solver s(gs);
    solver* solv = &s;
    bool solution;
    if (short_sol) {
        uint bound = 1;
        solver::sol_state ss;
        do {
            LOG_INFO("Depth: " << bound);
            solver s_(gs);
            solv = &s_;
            ss = s_.run_with_cutoff(none, bound++);
        } while (ss == solver::sol_state::cutoff);
        solution = ss == solver::sol_state::solved;
    } else {
        solution = solv->run() == solver::sol_state::solved;
    }
    ProfilerStop();

    if (solution) {
        if (!classify) solv->print_solution();
        cout << "Solved\n";
    } else {
        if (!classify) cout << "Deal:\n" << gs << "\n";
        cout << "No Possible Solution\n";
    }

    cout << "States Searched: " << solv->get_states_searched() << "\n";
    cout << "Final Depth: " << solv->get_final_depth() << "\n";
}

void calculate_solvability_percentage(const sol_rules& rules) {
    cout << "Calculating solvability percentage...\n\n"
            "[Lower Bound, Upper Bound] | (Solvable/Unsolvable/Intractable) "
            "| Current seed\n";
    cout << fixed << setprecision(3);

    int solvable = 0;
    int unsolvable = 0;
    int intractable = 0;

    for(int seed = 0; seed < INT_MAX; seed++) {
        game_state gs(rules, seed);
        solver sol(gs);
        atomic<bool> terminate_solver(false);

        future<bool> future = std::async(launch::async,
                                         [&sol, &terminate_solver](){
            return sol.run(terminate_solver) == solver::sol_state::solved;
        });

        future_status status;
        do {
            status = future.wait_for(chrono::seconds(45));

            if (status == future_status::timeout) {
                terminate_solver = true;
            } else if (status == future_status::ready) {
                if (terminate_solver) {
                    intractable++;
                } else if (future.get()) {
                    solvable++;
                } else {
                    unsolvable++;
                }
            }
        } while (status != future_status::ready);

        double lower_bound = sol_lower_bound(solvable, unsolvable, intractable);
        double upper_bound = sol_upper_bound(solvable, unsolvable, intractable);

        cout << "[" << lower_bound * 100 << ", " << upper_bound * 100
             << "] | ("<< solvable << "/" << unsolvable << "/" << intractable
             << ") | " << seed << "\n";
    }
}

double sol_lower_bound(int solvable, int unsolvable, int intractable) {
    int n = solvable + unsolvable + intractable;
    int x = solvable;
    double z = 1.96;
    bool lower_bound = true;
    return agrestiCoull(n, x, z, lower_bound);
}

double sol_upper_bound(int solvable, int unsolvable, int intractable) {
    int n = solvable + unsolvable + intractable;
    int x = solvable + intractable;
    double z = 1.96;
    bool lower_bound = false;
    return agrestiCoull(n, x, z, lower_bound);
}

double agrestiCoull(int n, int x, double z, bool lower_bound) {
    double n_ = n + pow(z, 2);
    double p = (x + pow(z, 2)/2)/n_;
    double v = z * sqrt((p - pow(p, 2))/n_);
    if (lower_bound) {
        double ans = p - v;
        return ans < 0 ? 0 : ans;
    } else {
        double ans = p + v;
        return ans > 1 ? 1 : ans;
    }
}
