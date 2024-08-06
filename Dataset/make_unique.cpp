#include <iostream>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <string>

using namespace std;

// Function to remove duplicate lines from the input file and save the unique lines to the output file
void remove_duplicate_lines(const string& input_filename, const string& output_filename) {
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

    unordered_set<string> unique_lines;
    string line;
    while (getline(infile, line)) {
        // Check if the line is unique
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
    string input_filename = "bitcoin-temporal-timeinstance.txt";
    string output_filename = "bitcoin-temporal-timeinstance-unique.txt";

    remove_duplicate_lines(input_filename, output_filename);

    return 0;
}
