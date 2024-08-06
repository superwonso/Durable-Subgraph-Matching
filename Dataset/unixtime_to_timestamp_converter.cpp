#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <ctime>

using namespace std;

// Function to read the dataset, convert the Unix time to human-readable timestamps, and save the result
void convert_and_save(const string& input_filename, const string& output_filename) {
    ifstream infile(input_filename);
    if (!infile.is_open()) {
        cerr << "Unable to open input file" << endl;
        return;
    }

    ofstream outfile(output_filename);
    if (!outfile.is_open()) {
        cerr << "Unable to open output file" << endl;
        infile.close();
        return;
    }

    string line;
    while (getline(infile, line)) {
        stringstream ss(line);
        vector<string> tokens;
        string token;

        // Split the line by space
        while (ss >> token) {
            tokens.push_back(token);
        }

        if (tokens.size() == 3) {
            // Convert the third token (Unix time) to a human-readable timestamp
            time_t unix_time = stoll(tokens[2]);
            tm* timestamp = localtime(&unix_time);
            char buffer[100];
            strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timestamp);
            
            // Save the line with the converted timestamp
            outfile << tokens[0] << " " << tokens[1] << " " << buffer << endl;
        }
    }

    infile.close();
    outfile.close();
}

int main() {
    // Example usage
    string input_filename = "bitcoin-temporal.txt";
    string output_filename = "bitcoin-temporal-timestamp.txt";
    convert_and_save(input_filename, output_filename);

    return 0;
}
