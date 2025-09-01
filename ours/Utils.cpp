#include "Utils.h"

// Function to compute the intersection of two unordered_sets (Time Instance Sets)
std::unordered_set<int> intersectTimeSets(const std::unordered_set<int>& set1, const std::unordered_set<int>& set2) {
    std::unordered_set<int> intersection;
    for (const auto& elem : set1) {
        if (set2.find(elem) != set2.end()) {
            intersection.insert(elem);
        }
    }
    return intersection;
}

// Bloom Filter Implementation

BloomFilter::BloomFilter(size_t size, size_t num_hashes) : size_(size), num_hashes_(num_hashes), bits_(size, false) {}

std::vector<size_t> BloomFilter::getHashes(int item) const {
    std::vector<size_t> hashes;
    for (size_t i = 0; i < num_hashes_; ++i) {
        size_t hash = hash_fn(item) + i * hash_fn(item + i);
        hashes.push_back(hash % size_);
    }
    return hashes;
}

void BloomFilter::add(int item) {
    std::vector<size_t> hashes = getHashes(item);
    for (auto& hash : hashes) {
        bits_[hash] = true;
    }
}

bool BloomFilter::possiblyContains(int item) const {
    std::vector<size_t> hashes = getHashes(item);
    for (auto& hash : hashes) {
        if (!bits_[hash]) {
            return false;
        }
    }
    return true;
}

void BloomFilter::clear() {
    std::fill(bits_.begin(), bits_.end(), false);
}

// Graph Utility Functions

namespace GraphUtils {
    bool hasEdge(const std::vector<std::vector<Edge>>& adj, int u, int v) {
        for (const auto& edge : adj[u]) {
            if (edge.to == v) {
                return true;
            }
        }
        return false;
    }

    int countDistinctNeighborLabels(const std::vector<std::vector<Edge>>& adj, int vertex) {
        std::unordered_set<std::string> unique_labels;
        for (const auto& edge : adj[vertex]) {
            unique_labels.insert(edge.label);
        }
        return unique_labels.size();
    }
}

// Function to read the temporal graph from a file
bool readTemporalGraph(const std::string& filename, Graph& graph) {
    std::ifstream infile(filename);
    if (!infile.is_open()) {
        std::cerr << "Error: Cannot open temporal graph file " << filename << std::endl;
        return false;
    }

    std::string line;

    // Random generator for labels
    std::vector<std::string> labels = {"A", "B", "C", "D", "E"};
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, labels.size() - 1);

    int max_vertex = -1;

    // Read edges and determine the maximum vertex ID
    while (getline(infile, line)) {
        if (line.empty()) continue;

        std::istringstream iss(line);    
        int v1, v2, time;    
        if (!(iss >> v1 >> v2 >> time)) {    
            std::cerr << "Error: Invalid line in temporal graph file: " << line << std::endl;    
            return false;    
        }

        max_vertex = std::max(max_vertex, std::max(v1, v2));
        
        // Ensure the graph structure can hold all vertices up to max_vertex
        while (graph.adj.size() <= max_vertex) {
            graph.adj.emplace_back();
            // Assign a random label to each new vertex
            graph.vertex_labels.push_back(labels[dis(gen)]);
        }

        // Add the edge between v1 and v2 with a random label
        graph.adj[v1].emplace_back(Edge{v2, labels[dis(gen)]});
        graph.edge_time_instances[{v1, v2}].insert(time);
    }
    graph.num_vertices = graph.adj.size();
    infile.close();

    return true;
}


// Function to read the query graph from a file
bool readQueryGraph(const std::string &filename ,Graph &queryGraph){
	std::ifstream infile(filename);	
	if(!infile.is_open()){
		std::cerr << "Error: Cannot open query graph file "<<filename<<std::endl;	
		return false;	
	}
        std::string line;

	while(getline(infile,line)){
		if(line.empty()) continue;

		std::istringstream iss(line);	
                std::string v1_str, v2_str, edge_label;
                if (!(iss >> v1_str >> v2_str)) {
                        std::cerr << "Error: Invalid line in query graph file: " << line << std::endl;
                        return false;
                }

                // Optional edge label
                if (iss >> edge_label) {
                        std::string extra;
                        if (iss >> extra) {
                                std::cerr << "Error: Invalid line in query graph file (too many tokens): " << line << std::endl;
                                return false;
                        }
                } else {
                        edge_label = "";
                }

                int v1 = -1, v2 = -1;

                auto it_v1 = std::find(queryGraph.vertex_labels.begin(), queryGraph.vertex_labels.end(), v1_str);
                if (it_v1 == queryGraph.vertex_labels.end()) {
                        v1 = queryGraph.vertex_labels.size();
                        queryGraph.vertex_labels.push_back(v1_str);
                        queryGraph.adj.emplace_back();
                } else {
                        v1 = it_v1 - queryGraph.vertex_labels.begin();
                }

                auto it_v2 = std::find(queryGraph.vertex_labels.begin(), queryGraph.vertex_labels.end(), v2_str);
                if (it_v2 == queryGraph.vertex_labels.end()) {
                        v2 = queryGraph.vertex_labels.size();
                        queryGraph.vertex_labels.push_back(v2_str);
                        queryGraph.adj.emplace_back();
                } else {
                        v2 = it_v2 - queryGraph.vertex_labels.begin();
                }

                queryGraph.adj[v1].emplace_back(Edge{v2, edge_label});
     }
	 queryGraph.num_vertices = queryGraph.adj.size();
     infile.close();

     return true;

}

