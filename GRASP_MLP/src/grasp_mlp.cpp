#include "linear_split/Pb_Data.h"
#include "linear_split/Split_Linear.h"
#include "metaheuristic/methods/giant_tour_creation.h"
#include "metaheuristic/methods/cw_savings.h"
#include "metaheuristic/methods/neighborhood.h"
#include "metaheuristic/methods/vnd.h"
#include "metaheuristic/methods/input_parser.h"
#include "metaheuristic/methods/preprocessing.h"
#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <ctime>
#include <iostream>
#include <limits>
#include <omp.h>
#include <set>
#include <random>
#include<chrono>
#include "metaheuristic/twiggy_core/decision_tree.h"
#include "metaheuristic/twiggy_core/random_forest.h"
#include "metaheuristic/twiggy_core/utils.h"
#define length(x) (sizeof(x) / sizeof(x[0]))

class RNG
{
public:
    typedef std::mt19937 Engine;
    typedef std::uniform_real_distribution<double> Distribution;
    int user_seed;

    RNG() : engines(), distribution(0.0, 1.0)
    {
        int threads = std::max(1, omp_get_max_threads());
        for(int seed = 0; seed < threads; ++seed)
        {
            engines.push_back(Engine((100*user_seed) + seed));
        }
    }

    double operator()()
    {
        int id = omp_get_thread_num();
        return distribution(engines[id]);
    }

    std::vector<Engine> engines;
    Distribution distribution;
};

using namespace std;

string direc_input = "../instances/";
string train_output_direc = "../results/solutions_datasets/";
string validation_output_direc = "../results/validation_solutions/";
string features_output = "../results/features_datasets/";
string evolution_output = "../results/evolution/";
string timing_direc = "../timing/";

vector<Neighborhood*> neighborhoods = {  new RelocateNeighborhood(false), new SwapNeighborhood(false), new TwoOptIntraNeighborhood(false), new TwoOptInterNeighborhood(false)};

vector<Neighborhood*> final_neighborhoods = { new SwapStarNeighborhood(false), new RelocationChainNeighborhood_LimitNodes(false), new CrossExchangeNeighborhood(false), new ThreeOptInterNeighborhood(false),};
//vector<Neighborhood*> final_neighborhoods = {new RelocationChainNeighborhood_LimitNodes(false), };
vector<vector<Neighborhood*>> neighborhood_combinations;

vector<vector<Neighborhood*>> neighborhood_combinations_sliced;

int indexes[] = { 0, 1, 2, 3};
vector<string> neighborhoods_names = { "r","s","t","T" };
//int indexes[] = { 0,  };
//vector<string> neighborhoods_names = { "r"};
vector<vector<string>> neighborhood_names_combinations;
vector<vector<string>> neighborhood_names_combinations_sliced;
int alpha = 3;

const int nThreads = 4;  // number of threads to use
unsigned int seeds[nThreads];

void seedThreads(int user_seed) {
    int my_thread_id;
    int seed;
#pragma omp parallel private (seed, my_thread_id)
    {
        my_thread_id = omp_get_thread_num();

        //create seed on thread using current time

        int seed = 100 * user_seed + my_thread_id;

        //munge the seed using our thread number so that each thread has its
        //own unique seed, therefore ensuring it will generate a different set of numbers
        seeds[my_thread_id] = (seed & 0xFFFFFFF0) | (my_thread_id + 1);

        printf("Thread %d has seed %u\n", my_thread_id, seeds[my_thread_id]);
    }

}

vector<vector<int>> deterministic_savings_function(vector<vector<float>> dist_matrix,
                                                   float mean_demands,
                                                   vector<int> demands,
                                                   bool use_new_savings)
{
    vector<vector<int>> savings;
    int s = 0;
    float lambda = 1.3;
    float mu = 0.6;
    float v = 0.6;
    for(int i = 1; i < dist_matrix.size(); i++) {
        for(int j = i + 1; j < dist_matrix.size(); j++) {
            // s = dist_matrix[i][0] + dist_matrix[0][j] -  dist_matrix[i][j]; //Original savings calculation
            if(use_new_savings == true) {
                s = dist_matrix[i][0] + dist_matrix[0][j] - (lambda * dist_matrix[i][j]) +
                    (mu * abs(dist_matrix[i][0] - dist_matrix[0][j])) + (v * (demands[i] + demands[j]) / mean_demands);
            } else {
                s = dist_matrix[i][0] + dist_matrix[0][j] - dist_matrix[i][j]; // Original savings calculation
            }
            savings.push_back({ s, static_cast<int>(-dist_matrix[i][j]), i, j });
        }
    }
    std::sort(savings.begin(), savings.end(),
              [](const std::vector<int>& a, const std::vector<int>& b) { return a[0] > b[0]; });

    return savings;
}


void run_search( int training_samples, int user_seed,  const string& instance_name, int method_id) {

    //Define files for storing execution results
    vector<string> splitting_result;
    boost::split(splitting_result, instance_name, boost::is_any_of("."));
    string global_results =
            validation_output_direc + "global_mlp_" + splitting_result[0] + "_s" + to_string(user_seed) ;
    string train_results =
            train_output_direc + "train_grasp_mlp_" + splitting_result[0] + "_s" + to_string(user_seed) + "_it" +
            to_string(training_samples);
    string validation_results =
            validation_output_direc + "validation_grasp_mlp_" + splitting_result[0] + "_s" + to_string(user_seed);

    string timing_results =
            timing_direc + "grasp_mlp_" + splitting_result[0] + "_s" + to_string(user_seed) ;
    string evolution_log =
            evolution_output + "grasp_mlp_with_final_evolution_" + splitting_result[0] + "_s" + to_string(user_seed)  + ".log";


    ofstream timing_output(timing_results);
    time_t grasp_time_start, grasp_time_end;
    time(&grasp_time_start);
    float create_features_time;
    auto global_time_begin = std::chrono::high_resolution_clock::now();

    Instance instance = Instance(direc_input + instance_name);

    bool use_new_savings = false;
    bool use_route_first = false;
    vector<vector<int>> savings;
    int construction_method = 1; //FORCE THE USE OF C&W CONSTRUCTION HEURISTICS
    if(construction_method == 1) {
        use_route_first = false;
        savings = deterministic_savings_function(
                instance.dist_matrix, instance.mean_demands, instance.demands, use_new_savings);
    } else {
        use_route_first = true;
    }
    vector<string> methods_vector = {"tsp", "vrp", "client-pairs", "tsp-vrp", "tsp-client-pairs", "vrp-client-pairs", "tsp-vrp-client-pairs"};
    string method = methods_vector[method_id];

    ofstream global_output(global_results);


    shared_ptr<Solution> train_best_solution = make_shared<Solution>();
    shared_ptr<Solution> train_best_initial_solution = make_shared<Solution>();
    shared_ptr<Solution> train_best_tsp_solution = make_shared<Solution>();
    bool is_train_best_solution_null = true; // Null references doesn't exists in c++
    int train_best_solution_neighborhood;
    float train_best_solution_cost = std::numeric_limits<float>::max();

    ofstream neighborhood_output(train_results);

    ofstream validation_output(validation_results);

    ofstream evolution_output(evolution_log);

    unordered_map<string, pair<float, float>> neighborhood_metrics;

    int train_global_iteration = 0;

    ostringstream neighborhoods_str_rep;
    vector<string> neighborhoods_ids = {};
    time_t evolution_time, train_time_start, train_time_end, validation_time_start,
            validation_time_end, last_evolution_time;

    float train_execution_time;
    float validation_execution_time;

    time(&last_evolution_time);
    time(&train_time_start);

    int current_iteration = 0;

    //RNG rand;

    //vector <float> vector_costs (neighborhood_combinations_sliced.size(), 0.0);

    //vector<shared_ptr<Solution>> initial_solutions_vector(grasp_iterations + 1, make_shared<Solution>());


    cout << "omp_get_max_threads(): " << omp_get_max_threads() << endl;
    seedThreads(user_seed);
    int tid;       // thread id when forking threads in for loop
    int seed;
    auto elapsed_time = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::high_resolution_clock::now() - global_time_begin).count();
    //const auto time_limit = instance.n_cust;
    auto time_limit = 60;
    int max_iterations = 999999999;

    if (instance.n_cust < 250){
        time_limit = instance.n_cust;
    }
    else if (instance.n_cust < 500){
        time_limit = 3 * instance.n_cust;
    }
    else if (instance.n_cust < 750){
        time_limit = 10 * instance.n_cust;
    }
    else {
        time_limit = 10 * instance.n_cust;
    }

    int skipped_solutions = 0;
    shared_ptr<Solution> best_initial_solution = make_shared<Solution>();
    shared_ptr<Solution> best_tsp_solution = make_shared<Solution>();
    shared_ptr<Solution> best_solution = make_shared<Solution>();
    float best_solution_cost = std::numeric_limits<float>::max();
    bool is_best_solution_null = true;
    int best_sol_neigh_string;
    vector<shared_ptr<SampleInfo>> sample_infos;
    int number_of_vnds = neighborhood_combinations_sliced.size();

    // Parameters for the random forest
    ImpurityMeasure impurity_measure = entropy;
    int min_samples_leaf = 1;
    int max_depth = 3;
    int min_samples_split = 2;
    int max_features = 8;
    MaxFeaturesMethod max_features_method = sqrt_method;
    double min_impurity_split = 0.0;
    int n_estimators = 100;
    //int max_samples = training_samples/6;
    int max_samples = -1;
    int n_threads = 1;
    max_features_method = log2_method;

    // train random forest on training data
    /*
    RandomForestClassifier random_forest_clf = RandomForestClassifier(
            n_estimators, impurity_measure, max_depth, min_samples_split,
            min_samples_leaf, max_features, max_features_method, min_impurity_split,
            n_threads, max_samples);
    */
    vector<shared_ptr <RandomForestClassifier>> classifiers;
    for (int i = 0; i < number_of_vnds; i++){
        shared_ptr<RandomForestClassifier> random_forest_clf = make_shared<RandomForestClassifier>(n_estimators, impurity_measure, max_depth, min_samples_split,
                                                                                                   min_samples_leaf, max_features, max_features_method, min_impurity_split,
                                                                                                   n_threads, max_samples);
        classifiers.push_back(random_forest_clf);
    }

// Here begins parallel iterations
#pragma omp parallel default(none) private(tid, seed) \
shared(training_samples, seeds,  instance, alpha, neighborhood_combinations_sliced, current_iteration, is_train_best_solution_null, \
train_best_solution, train_best_solution_cost, train_best_tsp_solution, train_best_initial_solution, train_best_solution_neighborhood, train_global_iteration, \
best_initial_solution, best_tsp_solution, best_solution, best_solution_cost, is_best_solution_null, best_sol_neigh_string, sample_infos, features_output, \
evolution_time, last_evolution_time, neighborhood_output, evolution_output, validation_output, grasp_time_start,  elapsed_time,  max_iterations, cout, global_time_begin, \
instance_name, user_seed, number_of_vnds, train_output_direc, validation_output_direc, method, classifiers, time_limit, skipped_solutions, use_route_first, savings)
    {
        tid = omp_get_thread_num();   // my thread id
        seed = seeds[tid];            // it is much faster to keep a private copy of our seed
        srand(seed);	              //seed rand_r or rand

        ParallelSavings* savings_sol = new ParallelSavings(seed, 90, savings);

        vector<GiantTour *> giant_tour_heuristics = {new Improved_GiantTour_RNN(seed), new GiantTour_RNI(seed),
                                                     new GiantTour_RBI(seed)};
        vector<float> vnds_cost_vector(neighborhood_combinations_sliced.size(), std::numeric_limits<float>::max());

        auto rand_engine = std::mt19937(seed);
	
	//cout << "omp_get_cancellation() " << omp_get_cancellation() <<endl;

#pragma omp for
        for (auto it = 0; it < max_iterations; it++) {

            shared_ptr<Solution> giant_tour_solution;
            shared_ptr<Solution> local_optima;
            shared_ptr<Solution> initial_solution;


            float local_optima_cost = std::numeric_limits<float>::max();


            Pb_Data *myData;
            Split_Linear *mySolver;

            initial_solution = make_shared<Solution>(instance, instance.n_cust);

            if (use_route_first == true) {
                int random_tour = rand() % giant_tour_heuristics.size();

                giant_tour_solution = giant_tour_heuristics[random_tour]->run(instance, alpha);
                giant_tour_solution->cost = giant_tour_solution->compute_cost(instance);
                initial_solution = giant_tour_solution;

                myData = new Pb_Data(instance, initial_solution->vehicles[0]->customers);

                myData->time_StartComput = clock();

                mySolver = new Split_Linear(myData);

                mySolver->solve();

                myData->time_EndComput = clock();

                vector<int> solution = myData->solution;
                vector<Client> cli = myData->cli;

                initial_solution = make_shared<Solution>(instance, myData->solutionNbRoutes);

                int cust_index = 1;
                for (unsigned int i = 0; i < myData->solutionNbRoutes; i++) {
                    if (i == myData->solutionNbRoutes - 1) {

                        initial_solution->vehicles[i] = make_shared<Vehicle>(instance.Q);

                        initial_solution->vehicles[i]->add_customer(initial_solution->customers[0]);

                        for (unsigned int j = solution[i]; j < cli.size(); j++) {
                            initial_solution->vehicles[i]->add_customer(initial_solution->customers[cli[j].index]);

                            cust_index++;
                        }
                    } else {

                        initial_solution->vehicles[i] = make_shared<Vehicle>(instance.Q);

                        initial_solution->vehicles[i]->add_customer(initial_solution->customers[0]);

                        for (unsigned int j = solution[i]; j < solution[i + 1]; j++) {

                            initial_solution->vehicles[i]->add_customer(initial_solution->customers[cli[j].index]);

                            cust_index++;
                        }

                        initial_solution->vehicles[i]->add_customer(initial_solution->customers[0]);
                    }
                    cust_index = 1;
                }

                initial_solution->cost = initial_solution->compute_cost(instance);

                for (unsigned int i = 0; i < initial_solution->vehicles.size(); i++) {
                    for (unsigned int j = 0; j < initial_solution->vehicles[i]->customers.size(); j++) {
                        shared_ptr<Customer> customer = initial_solution->vehicles[i]->customers[j];
                        customer->vehicle_route = i;
                    }
                }
            }
            else {
                initial_solution = savings_sol->clarke_and_wright(instance, initial_solution, 1, 100, 90, rand_engine);
                initial_solution->cost = initial_solution->compute_cost(instance);
                for (unsigned int i = 0; i < initial_solution->vehicles.size(); i++) {
                    for (unsigned int j = 0; j < initial_solution->vehicles[i]->customers.size(); j++) {
                        shared_ptr<Customer> customer = initial_solution->vehicles[i]->customers[j];
                        customer->vehicle_route = i;
                        customer->isRouted = true;
                    }
                }
            }

            if (use_route_first == false) {
                giant_tour_solution = initial_solution->create_giant_tour(instance);
            }


            for (unsigned int i = 0; i < neighborhood_combinations_sliced.size(); i++) {
                int relocation_applications = 0;
                local_optima = run_vnd(initial_solution, instance, neighborhood_combinations_sliced[i],
                                       relocation_applications);
                local_optima_cost = local_optima->cost;

                vnds_cost_vector[i] = local_optima_cost;

                if (is_best_solution_null || local_optima_cost < best_solution_cost) {
                    best_tsp_solution = giant_tour_solution;
                    best_initial_solution = initial_solution;
                    best_solution = local_optima;
                    best_sol_neigh_string = i;
                    best_solution_cost = local_optima_cost;
                    is_best_solution_null = false;
		    
                }
            }

#pragma omp critical
            {
                cout << "Termina la iteracion " << current_iteration++ << endl;
                neighborhood_output << "iteration: " << it << endl;
                neighborhood_output << "tsp-solution:\n" << giant_tour_solution->local_solutions().str() << endl;
                neighborhood_output << "initial-solution:\n" << initial_solution->local_solutions().str() << endl;
                for (unsigned int w = 0; w < vnds_cost_vector.size(); w++) {
                    neighborhood_output << "neighborhood-ordering: " << w << endl;
                    neighborhood_output << "final-solution-cost:\n" << to_string(vnds_cost_vector[w]) << endl;
                }
                if (is_train_best_solution_null || best_solution_cost < train_best_solution_cost) {
                    train_best_solution = best_solution;
                    train_best_tsp_solution = best_tsp_solution;
                    train_best_initial_solution = best_initial_solution;
                    train_best_solution_neighborhood = best_sol_neigh_string;
                    train_best_solution_cost = best_solution_cost;
                    is_train_best_solution_null = false;
                    train_global_iteration = it;
                    time(&evolution_time);
		    cout << "---------- Updated best_solution->cost: "<< best_solution_cost << endl;
                    evolution_output << "time: " << evolution_time - grasp_time_start << endl;
                    evolution_output << "cost: " << train_best_solution_cost << endl;
                }
                time(&evolution_time);
                if (evolution_time - last_evolution_time>= instance.n_cust/10) {
                    evolution_output << "time: " << evolution_time - grasp_time_start << endl;
                    evolution_output << "cost: " << train_best_solution_cost << endl;
                    time(&last_evolution_time);
                }
                shared_ptr<SampleInfo> sample_info = make_shared<SampleInfo>();
                sample_info->sample_number = it;
                sample_info->tsp_solution = giant_tour_solution;
                sample_info->initial_solution = initial_solution;
                sample_info->final_costs= vnds_cost_vector;
                sample_infos.push_back(sample_info);
            }
            elapsed_time = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::high_resolution_clock::now() - global_time_begin).count();
            if (elapsed_time < (0.25 * time_limit)) {
		
            }
            else{
                it = max_iterations + max_iterations ;
		#pragma omp cancel for
		#pragma omp flush(it)
            }
            if(use_route_first == true) {
                delete myData;
                delete mySolver;
            }
        }
#pragma omp single
        {
            string filename1 =
                    "train_" + instance_name + "_s" + to_string(user_seed) + "_it" + to_string(training_samples);
            DTOP_values train_features = print_vnd_features(sample_infos, instance, method, filename1, features_output,
                                                            number_of_vnds);
            //vector<table> data_frames (number_of_vnds);


            vector<vector<float>> temp_y(number_of_vnds, vector<float>(sample_infos.size(), 0.0));
            //vector<vector<float>> temp_labels(number_of_vnds, vector<float>(sample_infos.size(), 0.0));


            int good_solutions = (15 * sample_infos.size()) / 100;
            int bad_solutions = (50 * sample_infos.size()) / 100;
            cout << "Good solutions: " << good_solutions << endl;
            cout << "Bad solutions: " << bad_solutions << endl;
            vector<float> good_costs(number_of_vnds, 0);
            vector<float> bad_costs(number_of_vnds, 0);

            //vector<vector<int>> labels(number_of_vnds, vector<int>(good_solutions+bad_solutions, 0));

            for (int w = 0; w < number_of_vnds; w++) {
                vector<int> labels;
                table data_frames;
                //data_frames[w].headers_ = train_features.features_names;
                data_frames.headers_ = train_features.features_names;
                for (int i = 0; i < sample_infos.size(); i++) {
                    temp_y[w][i] = (sample_infos[i]->final_costs[w]);
                }
                std::nth_element(temp_y[w].begin(), temp_y[w].begin() + good_solutions, temp_y[w].end());
                good_costs[w] = temp_y[w][good_solutions];
                cout << "Good cost: " << good_costs[w] << endl;
                std::nth_element(temp_y[w].begin(), temp_y[w].begin() + bad_solutions, temp_y[w].end());
                bad_costs[w] = temp_y[w][bad_solutions];
                cout << "Bad cost: " << bad_costs[w] << endl;


                for (int i = 0; i < sample_infos.size(); i++) {
                    if (sample_infos[i]->final_costs[w] <= good_costs[w]) {
                        //data_frames[w].AddRow(train_features.features[i]);
                        data_frames.AddRow(train_features.features[i]);
                        //labels[w][i] = 1;
                        labels.push_back(1);
                    } else if (sample_infos[i]->final_costs[w] >= bad_costs[w]) {
                        //data_frames[w].AddRow(train_features.features[i]);
                        data_frames.AddRow(train_features.features[i]);
                        //labels[w][i] = 1;
                        labels.push_back(0);
                    }
                }
                // train random forest on training data
                cout << "Train random forest on training data" << endl;
                //classifiers[w]->BuildForest(data_frames[w].data_, labels[w]);
                classifiers[w]->BuildForest(data_frames.data_, labels);
                classifiers[w]->class_weight.push_back(float(good_solutions + bad_solutions)/float(2 *  bad_solutions)); //Append Class weight for bad solutions
                classifiers[w]->class_weight.push_back(float(good_solutions + bad_solutions)/float(2 * good_solutions)); //Append Class weight for good solutions
                //classifiers[w]->class_weight.push_back(1); //Append Class weight for bad solutions
                //classifiers[w]->class_weight.push_back(1); //Append Class weight for good solutions
                cout << "Train done" << endl;

            }

            cout << "n_labels " << classifiers[0]->n_labels_ << endl;
            cout << "class_weights: Bad sols: " << classifiers[0]->class_weight[0] << " Good sols: "<< classifiers[0]->class_weight[1] << endl;


        }
#pragma omp for
        for (auto it = training_samples; it < max_iterations; it++) {

            shared_ptr<Solution> giant_tour_solution;
            shared_ptr<Solution> local_optima;
            shared_ptr<Solution> initial_solution;


            float neighborhood_execution_time;
            float local_optima_cost = std::numeric_limits<float>::max();


            Pb_Data *myData;
            Split_Linear *mySolver;

            initial_solution = make_shared<Solution>(instance, instance.n_cust);

            if (use_route_first == true) {
                Pb_Data *myData;
                Split_Linear *mySolver;

            int random_tour = rand() % giant_tour_heuristics.size();

            giant_tour_solution = giant_tour_heuristics[random_tour]->run(instance, alpha);
            giant_tour_solution->cost = giant_tour_solution->compute_cost(instance);
            initial_solution = giant_tour_solution;

            myData = new Pb_Data(instance, initial_solution->vehicles[0]->customers);

            myData->time_StartComput = clock();

            mySolver = new Split_Linear(myData);

            mySolver->solve();

            myData->time_EndComput = clock();

            vector<int> solution = myData->solution;
            vector<Client> cli = myData->cli;

            initial_solution = make_shared<Solution>(instance, myData->solutionNbRoutes);

            int cust_index = 1;
            for (unsigned int i = 0; i < myData->solutionNbRoutes; i++) {
                if (i == myData->solutionNbRoutes - 1) {

                    initial_solution->vehicles[i] = make_shared<Vehicle>(instance.Q);

                    initial_solution->vehicles[i]->add_customer(initial_solution->customers[0]);

                    for (unsigned int j = solution[i]; j < cli.size(); j++) {
                        initial_solution->vehicles[i]->add_customer(initial_solution->customers[cli[j].index]);

                        cust_index++;
                    }
                } else {

                    initial_solution->vehicles[i] = make_shared<Vehicle>(instance.Q);

                    initial_solution->vehicles[i]->add_customer(initial_solution->customers[0]);

                    for (unsigned int j = solution[i]; j < solution[i + 1]; j++) {

                        initial_solution->vehicles[i]->add_customer(initial_solution->customers[cli[j].index]);

                        cust_index++;
                    }

                    initial_solution->vehicles[i]->add_customer(initial_solution->customers[0]);
                }
                cust_index = 1;
            }

            initial_solution->cost = initial_solution->compute_cost(instance);

            for (unsigned int i = 0; i < initial_solution->vehicles.size(); i++) {
                for (unsigned int j = 0; j < initial_solution->vehicles[i]->customers.size(); j++) {
                    shared_ptr<Customer> customer = initial_solution->vehicles[i]->customers[j];
                    customer->vehicle_route = i;
                }
            }

            }
            else {
                //initial_solution = savings_sol->run(initial_solution, instance, 90, savings);
                initial_solution = savings_sol->clarke_and_wright(instance, initial_solution, 1, 100, 90, rand_engine);
                initial_solution->cost = initial_solution->compute_cost(instance);
                for (unsigned int i = 0; i < initial_solution->vehicles.size(); i++) {
                    for (unsigned int j = 0; j < initial_solution->vehicles[i]->customers.size(); j++) {
                        shared_ptr<Customer> customer = initial_solution->vehicles[i]->customers[j];
                        customer->vehicle_route = i;
                        customer->isRouted = true;
                    }
                }
            }

            if (use_route_first == false) {
                giant_tour_solution = initial_solution->create_giant_tour(instance);
            }

            vector<shared_ptr<SampleInfo>> solution_infos;
            shared_ptr<SampleInfo> sample_info = make_shared<SampleInfo>();
            sample_info->sample_number = it;
            sample_info->tsp_solution = giant_tour_solution;
            sample_info->initial_solution = initial_solution;
            solution_infos.push_back(sample_info);

            DTOP_values solution_features = create_solution_features(solution_infos, instance, method);
            table solution_data;
            solution_data.headers_ = solution_features.features_names;
            solution_data.AddRow(solution_features.features[0]);

            bool applied_vnd = false;

            for (unsigned int i = 0; i < neighborhood_combinations_sliced.size(); i++) {
                vector<int> predicted_classes_rf;
                predicted_classes_rf = classifiers[i]->PredictClasses(&solution_data.data_);

                if (predicted_classes_rf[0] > 0) {
                    applied_vnd = true;
                    int relocation_applications = 0;
                    local_optima = run_vnd(initial_solution, instance, neighborhood_combinations_sliced[i],
                                           relocation_applications);
                    local_optima_cost = local_optima->cost;

                    vnds_cost_vector[i] = local_optima_cost;

                    if (is_best_solution_null || local_optima_cost < best_solution_cost) {
                        best_tsp_solution = giant_tour_solution;
                        best_initial_solution = initial_solution;
                        best_solution = local_optima;
                        best_sol_neigh_string = i;
                        best_solution_cost = local_optima_cost;
                        is_best_solution_null = false;
			
                    }
                }
                elapsed_time = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::high_resolution_clock::now() - global_time_begin).count();
            }
            if (elapsed_time < time_limit) {
#pragma omp critical
                {
                    if (applied_vnd == true){
                        cout << "Termina la iteracion " << current_iteration++ << endl;
                        validation_output << "iteration: " << it << endl;
                        validation_output << "tsp-solution:\n" << giant_tour_solution->local_solutions().str() << endl;
                        validation_output << "initial-solution:\n" << initial_solution->local_solutions().str() << endl;


                        for (unsigned int w = 0; w < vnds_cost_vector.size(); w++) {
                            validation_output << "neighborhood-ordering: " << w << endl;
                            validation_output << "final-solution-cost:\n" << to_string(vnds_cost_vector[w]) << endl;
                            vnds_cost_vector[w] = std::numeric_limits<float>::max();
                        }

                        if (is_train_best_solution_null || best_solution_cost < train_best_solution_cost) {
                            train_best_solution = best_solution;
                            train_best_tsp_solution = best_tsp_solution;
                            train_best_initial_solution = best_initial_solution;
                            train_best_solution_neighborhood = best_sol_neigh_string;
                            train_best_solution_cost = best_solution_cost;
                            is_train_best_solution_null = false;
                            train_global_iteration = it;
                            time(&evolution_time);
			    cout << "---------- Updated best_solution->cost: "<< best_solution_cost << endl;
                            evolution_output << "time: " << evolution_time - grasp_time_start << endl;
                            evolution_output << "cost: " << train_best_solution_cost << endl;
                        }

                    }
                    else {
                        skipped_solutions++;
                    }
                    time(&evolution_time);
                    if (evolution_time - last_evolution_time >= instance.n_cust/10) {
                        evolution_output << "time: " << evolution_time - grasp_time_start << endl;
                        evolution_output << "cost: " << train_best_solution_cost << endl;
                        time(&last_evolution_time);
                    }
                }
            }
            else{
                it = max_iterations;
		#pragma omp cancel for
#pragma omp flush(it)
            }
            if(use_route_first == true) {
                delete myData;
                delete mySolver;
            }
        }
        delete savings_sol;
        for (auto giant_tour: giant_tour_heuristics) {
            delete giant_tour;
        }
    }

    time(&train_time_end);
    train_execution_time = train_time_end - train_time_start;
    time(&validation_time_start);


    neighborhood_output << string(100, '-') << endl;
    neighborhood_output << "Execution time per neighborhood:" << endl;


    neighborhood_output.close();
    validation_output.close();
    evolution_output.close();

    time(&grasp_time_end);
    time(&validation_time_end);
    validation_execution_time = validation_time_end - validation_time_start;


    timing_output << "create solutions total time: " << to_string(elapsed_time) << endl;
    timing_output.close();

// global_output << "neighborhood-ordering: " << train_best_solution_neighborhood << endl;
    global_output << "neighborhood-ordering: " << train_best_solution_neighborhood << endl;
    global_output << "train_sample_number: " << to_string(train_global_iteration) << endl;
    global_output << "train_neighboorhood-execution-time: " << 0 << endl;
    global_output << "train_tsp-solution: " << endl << train_best_tsp_solution->local_solutions().str() << endl;
//ostringstream* temp_sol = train_best_initial_solution->local_solutions();
    global_output << "train_initial-solution: " << endl << train_best_initial_solution->local_solutions().str() << endl;
    global_output << "train_final-solution: " << endl << train_best_solution->local_solutions().str() << endl;
    global_output << "train_total time: " << to_string(train_execution_time) << endl;


    global_output << "validation_total time: " << to_string(validation_execution_time) << endl;

    global_output << "\n train_final_cost: " << to_string(train_best_solution->cost) << endl;
    global_output << "train_total time: " << to_string(train_execution_time) << endl;
    global_output << "validation_total time: " << to_string(validation_execution_time) << endl;

// global_output << "final_cost: " << to_string(train_best_solution_cost) << endl;
// global_output << "seed: " << to_string(seed) << endl;
// global_output << "Total GRASP time: " << to_string(grasp_execution_time) << endl;

    global_output.close();

    cout << "Total time: " << train_execution_time << endl;
    cout << "Best solution cost: " << train_best_solution->compute_cost(instance) << endl;

    cout << "skipped_solutions: " << skipped_solutions << endl;

}


int main(int argc, char* argv[])
{

    do {
        vector<Neighborhood*> combination;
        vector<string> names_combination;
        for(unsigned int i = 0; i < length(indexes); i++) {
            // cout << neighborhoods[indexes[i]]->id << "-";
            combination.push_back(neighborhoods[indexes[i]]);
            names_combination.push_back(neighborhoods_names[indexes[i]]);
            //cout << neighborhoods_names[indexes[i]];
        }
        // cout << endl;
        // combination.push_back(new SwapStarNeighborhood(false));
        //combination.push_back(new RelocationChainNeighborhood_LimitNodes(false));
        //combination.push_back(new RelocationChainNeighborhood_LimitDepth(false));
        //cout << " " << endl;
        neighborhood_combinations.push_back(combination);
        neighborhood_names_combinations.push_back(names_combination);
    } while(std::next_permutation(begin(indexes), end(indexes)));

    for(unsigned int i = 0; i < neighborhood_combinations.size(); i += 1) {
        if (i%7==0){
            //vector<Neighborhood*> final_neighborhoods = { new RelocationChainNeighborhood_LimitNodes(false), new RelocationChainNeighborhood_LimitDepth(false), \
				new SwapStarNeighborhood(false), new CrossExchangeNeighborhood(false) };
            //for (unsigned int j = 0; j < final_neighborhoods.size(); j++)
            //{
            neighborhood_combinations[i].push_back(final_neighborhoods[0]);
            neighborhood_combinations_sliced.push_back(neighborhood_combinations[i]);
            // neighborhood_combinations[i].pop_back();
            //neighborhood_names_combinations_sliced.push_back(neighborhood_names_combinations[i]);
            //}
            neighborhood_names_combinations_sliced.push_back(neighborhood_names_combinations[i]);
        }
    }


    string train_samples = argv[1];
    size_t pos3;
    int training_samples = stoi(train_samples, &pos3);
    string n_seed = argv[2];
    size_t pos2;
    int seed = stoi(n_seed, &pos2);
    string method_str = argv[3];
    size_t pos5;
    int method_id = stoi(method_str, &pos5);

    string instance_name = argv[4];

    run_search(training_samples, seed, instance_name, method_id);
    for (auto neighborhood: neighborhoods){
        delete neighborhood;
    }
    for (auto neighborhood: final_neighborhoods){
        delete neighborhood;
    }

    cout << "Sali de run_search" << endl;
}
