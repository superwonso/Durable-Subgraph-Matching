#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <ctime>
#include <unordered_set>

using namespace std;

// Function to calculate the time instance based on the base time and interval
int calculate_time_instance(time_t base_time, time_t current_time, int interval_minutes) {
    return (current_time - base_time) / (interval_minutes * 60) + 1;
}

// Function to read the dataset, calculate time instances, and save the result
void convert_and_save_with_time_instance(const string& input_filename, const string& temp_output_filename, int interval_minutes) {
    ifstream infile(input_filename);
    if (!infile.is_open()) {
        cerr << "Unable to open input file" << endl;
        return;
    }

    ofstream outfile(temp_output_filename);
    if (!outfile.is_open()) {
        cerr << "Unable to open output file" << endl;
        infile.close();
        return;
    }

    string line;
    vector<time_t> unix_times;
    while (getline(infile, line)) {
        stringstream ss(line);
        vector<string> tokens;
        string token;

        // Split the line by space
        while (ss >> token) {
            tokens.push_back(token);
        }

        if (tokens.size() == 3) {
            // Convert the third token (Unix time) to a time_t value
            time_t unix_time = stoll(tokens[2]);
            unix_times.push_back(unix_time);
        }
    }

    if (unix_times.empty()) {
        cerr << "No valid data in input file" << endl;
        infile.close();
        outfile.close();
        return;
    }

    // Use the first timestamp as the base time
    time_t base_time = unix_times[0];
    base_time -= base_time % (interval_minutes * 60); // Adjust base time to the nearest lower interval

    // Re-read the file to output the results with time instances
    infile.clear();
    infile.seekg(0, ios::beg);
    while (getline(infile, line)) {
        stringstream ss(line);
        vector<string> tokens;
        string token;

        // Split the line by space
        while (ss >> token) {
            tokens.push_back(token);
        }

        if (tokens.size() == 3) {
            // Convert the third token (Unix time) to a time_t value
            time_t unix_time = stoll(tokens[2]);

            // Calculate the time instance
            int time_instance = calculate_time_instance(base_time, unix_time, interval_minutes);

            // Save the line with the time instance replacing the Unix time
            outfile << tokens[0] << " " << tokens[1] << " " << time_instance << endl;
        }
    }

    infile.close();
    outfile.close();
}

// Function to remove duplicate lines from the output file
void remove_duplicates(const string& temp_output_filename, const string& final_output_filename) {
    ifstream infile(temp_output_filename);
    if (!infile.is_open()) {
        cerr << "Unable to open temp output file" << endl;
        return;
    }

    ofstream outfile(final_output_filename);
    if (!outfile.is_open()) {
        cerr << "Unable to open final output file" << endl;
        infile.close();
        return;
    }

    unordered_set<string> unique_lines;
    string line;
    while (getline(infile, line)) {
        if (unique_lines.find(line) == unique_lines.end()) {
            unique_lines.insert(line);
            outfile << line << endl;
        }
    }

    infile.close();
    outfile.close();
}

int main() {
    // Example usage
    string input_filename = "bitcoin-temporal.txt";
    string temp_output_filename = "bitcoin-temporal-timeinstance-1w.txt";
    string final_output_filename = "bitcoin-temporal-timeinstance-unique-1w.txt";
    int interval_minutes = 10080;

    convert_and_save_with_time_instance(input_filename, temp_output_filename, interval_minutes);
    remove_duplicates(temp_output_filename, final_output_filename);

    // Optionally, remove the temporary file
    // remove(temp_output_filename.c_str());

    return 0;
}
