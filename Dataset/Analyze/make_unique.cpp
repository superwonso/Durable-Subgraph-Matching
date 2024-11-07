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

int main(int argc, char *argv[]) {
    // Check if correct number of arguments are provided
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <input_file> <output_file>" << std::endl;
        return 1;
    }

    // Get filenames from command line arguments
    string input_filename = argv[1];
    string output_filename = argv[2];

    remove_duplicate_lines(input_filename, output_filename);

    return 0;
}