/*=============================================================================
# Filename: util.h
# Author: Bookug Lobert
# Mail: zengli-bookug@pku.edu.cn
# Last Modified: 2018-03-11 16:31
# Description:
The original code only deals with undirected graphs, each vertex has many labels.
Here we want to deal with directed graphs with edge labels, a simple strategy is
to treat graphs as undirected graphs without edge labels first, do subgraph isomorphism
with TurboISO, and finally verify each answer according to the real graph restrictions.
=============================================================================*/

#ifndef _UTIL_H
#define _UTIL_H

#include <iostream>
#include <vector>
#include <time.h>
#include <queue>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <limits.h>
#include <float.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <regex.h>
#include <locale.h>
#include <assert.h>
#include <libgen.h>
#include <signal.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/stat.h>

/* ---- for Linux headers ----
// #include <sys/mman.h>
// #include <sys/wait.h>

// #include <sys/socket.h>
// #include <netinet/in.h>
// #include <netdb.h>
// #include <arpa/inet.h>
---- */

#ifdef _WIN32
#include <windows.h>
#include <synchapi.h>
#include <psapi.h>
#endif

//NOTICE:below are restricted to C++, C files should not include(maybe nested) this header!
#include <bitset>
#include <string>
#include <fstream>
#include <iostream>
#include <sstream>

#include <map>
#include <set>
#include <stack>
#include <queue>
#include <deque>
#include <vector>
#include <list>
#include <iterator>
#include <algorithm>
#include <functional>
#include <utility>
#include <new>

//NOTICE:below are libraries need to link
#include <thread>    //only for c++11 or greater versions
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <future>
#include <memory>
#include <stdexcept>
#include <chrono>
#include <cstdint>
#include <pthread.h>
#include <math.h>
#include <readline/readline.h>
#include <readline/history.h>

//Below are for boost
//Added for the json-example
//#define BOOST_SPIRIT_THREADSAFE
//#include <boost/spirit.hpp>
//#include <boost/property_tree/ptree.hpp>
//#include <boost/property_tree/json_parser.hpp>

//Added for the default_resource example
//#include <boost/filesystem.hpp>
//#include <boost/regex.hpp>
//#include <boost/thread/thread.hpp>
//#include <boost/bind.hpp>
//#include <boost/asio.hpp>
//#include <boost/utility/string_ref.hpp>
//#include <boost/algorithm/string/predicate.hpp>
//#include <boost/functional/hash.hpp>
//#include <unordered_map>
//#include <random>
//#include <type_traits>

#include "Graph.h"

using namespace std;

int sizebool = sizeof(bool);
int sizeint = sizeof(int);


struct Child
{
	int s;
	int e;
};

struct labelVlist
{
	int label;
	vector <int> vlist;
    bool operator<(const labelVlist& lv) const
    {
        return this->label < lv.label;
    }
    bool operator==(const labelVlist& lv) const
    {
        return this->label == lv.label;
    }
};

enum class NativeParseStatus
{
    Loaded,
    End,
    Error
};

struct NativeEdgeRecord
{
    int from;
    int to;
    int label;
};

struct NativeGraphRecord
{
    int vertex_count = 0;
    int edge_count = 0;
    int vertex_label_count = 0;
    int edge_label_count = 1;
    vector<vector<int>> vertex_labels;
    vector<NativeEdgeRecord> edges;
};

bool nativeLineIsBlank(const string& line)
{
    return line.find_first_not_of(" \t\r") == string::npos;
}

map<FILE*, string> pending_native_lines;

bool readNativeLine(FILE* fp, string& line)
{
    line.clear();
    if(fp == NULL) return false;
    const auto pending = pending_native_lines.find(fp);
    if(pending != pending_native_lines.end())
    {
        line = pending->second;
        pending_native_lines.erase(pending);
        return true;
    }
    int character = 0;
    while((character = fgetc(fp)) != EOF)
    {
        if(character == '\n') break;
        if(character != '\r') line.push_back(static_cast<char>(character));
    }
    return character != EOF || !line.empty();
}

bool parseNativeMarker(const string& line, long long& graph_id)
{
    istringstream parser(line);
    char type = '\0';
    string hash;
    string extra;
    return (parser >> type >> hash >> graph_id) &&
           !(parser >> extra) && type == 't' && hash == "#";
}

bool parseNativeHeader(
    const string& line,
    bool query,
    NativeGraphRecord& record)
{
    istringstream parser(line);
    long long vertices = 0;
    long long edges = 0;
    long long vertex_labels = 0;
    long long edge_labels = 1;
    string extra;
    const bool parsed = query
        ? static_cast<bool>(parser >> vertices >> edges >> vertex_labels >> edge_labels)
        : static_cast<bool>(parser >> vertices >> edges >> vertex_labels);
    if(!parsed || (parser >> extra) ||
       vertices <= 0 || vertices > INT_MAX ||
       edges < 0 || edges > INT_MAX ||
       vertex_labels <= 0 || vertex_labels > INT_MAX ||
       edge_labels <= 0 || edge_labels > INT_MAX)
    {
        return false;
    }
    record.vertex_count = static_cast<int>(vertices);
    record.edge_count = static_cast<int>(edges);
    record.vertex_label_count = static_cast<int>(vertex_labels);
    record.edge_label_count = static_cast<int>(edge_labels);
    return true;
}

NativeParseStatus readNativeGraph(
    FILE* fp,
    bool query,
    NativeGraphRecord& record,
    const char* graph_kind)
{
    record = NativeGraphRecord{};
    string line;
    long long line_number = 0;
    bool found_marker = false;
    long long graph_id = -2;

    while(readNativeLine(fp, line))
    {
        ++line_number;
        if(nativeLineIsBlank(line)) continue;
        if(!parseNativeMarker(line, graph_id))
        {
            cerr << "Error: malformed " << graph_kind
                 << " graph marker at line " << line_number << ".\n";
            return NativeParseStatus::Error;
        }
        found_marker = true;
        break;
    }
    if(!found_marker)
    {
        cerr << "Error: " << graph_kind << " graph file ended without t # -1.\n";
        return NativeParseStatus::Error;
    }
    if(graph_id == -1) return NativeParseStatus::End;
    if(graph_id < 0 || graph_id > INT_MAX)
    {
        cerr << "Error: invalid " << graph_kind << " graph ID.\n";
        return NativeParseStatus::Error;
    }

    bool found_header = false;
    while(readNativeLine(fp, line))
    {
        ++line_number;
        if(nativeLineIsBlank(line)) continue;
        found_header = parseNativeHeader(line, query, record);
        break;
    }
    if(!found_header)
    {
        cerr << "Error: invalid or missing " << graph_kind
             << " graph header: '" << line << "'.\n";
        return NativeParseStatus::Error;
    }

    try
    {
        record.vertex_labels.resize(static_cast<std::size_t>(record.vertex_count));
        record.edges.reserve(static_cast<std::size_t>(record.edge_count));
    }
    catch(const bad_alloc&)
    {
        cerr << "Error: " << graph_kind << " graph header requests too much memory.\n";
        return NativeParseStatus::Error;
    }

    int vertex_lines = 0;
    int edge_lines = 0;
    while(true)
    {
        if(!readNativeLine(fp, line))
        {
            cerr << "Error: " << graph_kind
                 << " graph ended before the next t marker (missing t # -1).\n";
            return NativeParseStatus::Error;
        }
        ++line_number;
        if(nativeLineIsBlank(line)) continue;

        istringstream parser(line);
        char type = '\0';
        parser >> type;
        if(type == 't')
        {
            long long next_id = -2;
            if(!parseNativeMarker(line, next_id) ||
               (next_id < 0 && next_id != -1) || next_id > INT_MAX)
            {
                cerr << "Error: malformed " << graph_kind
                     << " graph boundary at line " << line_number << ".\n";
                return NativeParseStatus::Error;
            }
            pending_native_lines[fp] = line;
            break;
        }

        if(type == 'v')
        {
            long long id = -1;
            long long label = -1;
            string extra;
            if(!(parser >> id >> label) || (parser >> extra) ||
               id < 0 || id >= record.vertex_count ||
               label <= 0 || label > record.vertex_label_count)
            {
                cerr << "Error: invalid " << graph_kind
                     << " vertex at line " << line_number << ".\n";
                return NativeParseStatus::Error;
            }
            const std::size_t index = static_cast<std::size_t>(id);
            vector<int>& labels = record.vertex_labels[index];
            if(query && !labels.empty())
            {
                cerr << "Error: duplicate " << graph_kind
                     << " vertex ID " << id << ".\n";
                return NativeParseStatus::Error;
            }
            if(find(labels.begin(), labels.end(), static_cast<int>(label)) != labels.end())
            {
                cerr << "Error: duplicate " << graph_kind
                     << " vertex ID/label pair.\n";
                return NativeParseStatus::Error;
            }
            labels.push_back(static_cast<int>(label));
            ++vertex_lines;
            continue;
        }

        if(type == 'e')
        {
            long long from = -1;
            long long to = -1;
            long long label = 1;
            string extra;
            const bool parsed = query
                ? static_cast<bool>(parser >> from >> to >> label)
                : static_cast<bool>(parser >> from >> to);
            if(!parsed || (parser >> extra) ||
               from < 0 || from >= record.vertex_count ||
               to < 0 || to >= record.vertex_count || from == to ||
               label <= 0 || label > record.edge_label_count)
            {
                cerr << "Error: invalid " << graph_kind
                     << " edge at line " << line_number << ".\n";
                return NativeParseStatus::Error;
            }
            record.edges.push_back({
                static_cast<int>(from), static_cast<int>(to), static_cast<int>(label)});
            ++edge_lines;
            continue;
        }

        cerr << "Error: unknown " << graph_kind
             << " record type at line " << line_number << ".\n";
        return NativeParseStatus::Error;
    }

    const bool missing_vertex = any_of(
        record.vertex_labels.begin(), record.vertex_labels.end(),
        [](const vector<int>& labels) { return labels.empty(); });
    if((query && vertex_lines != record.vertex_count) ||
       edge_lines != record.edge_count || missing_vertex)
    {
        cerr << "Error: " << graph_kind
             << " graph header counts do not match its records.\n";
        return NativeParseStatus::Error;
    }

    sort(
        record.edges.begin(), record.edges.end(),
        [](const NativeEdgeRecord& lhs, const NativeEdgeRecord& rhs) {
            if(lhs.from != rhs.from) return lhs.from < rhs.from;
            if(lhs.to != rhs.to) return lhs.to < rhs.to;
            return lhs.label < rhs.label;
        });
    const auto duplicate = adjacent_find(
        record.edges.begin(), record.edges.end(),
        [](const NativeEdgeRecord& lhs, const NativeEdgeRecord& rhs) {
            return lhs.from == rhs.from && lhs.to == rhs.to &&
                   lhs.label == rhs.label;
        });
    if(duplicate != record.edges.end())
    {
        cerr << "Error: duplicate " << graph_kind << " directed edge.\n";
        return NativeParseStatus::Error;
    }
    return NativeParseStatus::Loaded;
}

//a label set for each vertex
class Graph							//G'
{
public:
	int LabelNum;
	int numVertex;

	vector <int> *vList;			//a label list of each veretx

	vector <int> *labelList;		//inverse label list

	vector <labelVlist> *graList;	//edge info

	DGraph* real_graph;   //directed graph with edge labels

	Graph()
	{
		vList = NULL;
		labelList = NULL;
		graList = NULL;
		real_graph = NULL;
	}

	~Graph()
	{
		if(vList != NULL)
			delete []vList;
		if(labelList != NULL)
			delete []labelList;
		if(graList != NULL)
			delete []graList;
		if(real_graph != NULL)
		{
			delete real_graph;
		}
	}

    void transform()
    {
        //BETTER: sort vertices in labelList
        for(int i = 0; i < numVertex; ++i)
        {
            sort(graList[i].begin(), graList[i].end());
            for(std::size_t j = 0; j < graList[i].size(); ++j)
            {
                vector<int>& vl = graList[i][j].vlist;
                sort(vl.begin(), vl.end());
                vl.erase(unique(vl.begin(), vl.end()), vl.end());
            }
        }
    }

	void init(int _numVertex, int _LabelNum)
	{
		LabelNum = _LabelNum;
		numVertex = _numVertex;

		vList = new vector <int> [numVertex];

		labelList = new vector <int> [LabelNum];

		graList = new vector <labelVlist> [numVertex];
	}

	void addVertex(int v, int l)
	{
		vList[v].push_back(l);
		labelList[l].push_back(v);
	}

	int contain(int e, vector <labelVlist> *temp)
	{
		int Size = temp->size();
		for(int i = 0; i < Size; i ++)
		{
			if((*temp)[i].label == e)
				return i;
		}
		return -1;
	}

	void addEdge(int from, int to)
	{
		int Size = vList[to].size();
		for(int i = 0; i < Size; i ++)   //enumerate each label of to
		{
			int label = vList[to][i];
			int pos = contain(label, &(graList[from]));
			if(pos != -1)
			{
				graList[from][pos].vlist.push_back(to);
			}
			else
			{
				labelVlist l;
				l.label = label;
				l.vlist.push_back(to);
				graList[from].push_back(l);
			}
		}

		Size = vList[from].size();
		for(int i = 0; i < Size; i ++)
		{
			int label = vList[from][i];
			int pos = contain(label, &(graList[to]));
			if(pos != -1)
			{
				graList[to][pos].vlist.push_back(from);
			}
			else
			{
				labelVlist l;
				l.label = label;
				l.vlist.push_back(from);
				graList[to].push_back(l);
			}
		}
	}

	NativeParseStatus createGraph(FILE *fp)
	{
		NativeGraphRecord record;
		const NativeParseStatus status =
			readNativeGraph(fp, false, record, "data");
		if(status != NativeParseStatus::Loaded)
			return status;
		try
		{
			this->numVertex = record.vertex_count;
			this->LabelNum = record.vertex_label_count;
			vList = new vector<int>[numVertex];
			labelList = new vector<int>[LabelNum + 1];
			graList = new vector<labelVlist>[numVertex];
			this->real_graph = new DGraph;
			this->real_graph->vertices.resize(static_cast<std::size_t>(numVertex));
			for(int id = 0; id < numVertex; ++id)
			{
				const vector<int>& labels =
					record.vertex_labels[static_cast<std::size_t>(id)];
				for(int label : labels) addVertex(id, label);
				this->real_graph->vertices[static_cast<std::size_t>(id)].label = labels.front();
			}
			for(const NativeEdgeRecord& edge : record.edges)
			{
				addEdge(edge.from, edge.to);
				this->real_graph->addEdge(edge.from, edge.to, 1);
			}
		}
		catch(const bad_alloc&)
		{
			cerr << "Error: cannot allocate data graph.\n";
			return NativeParseStatus::Error;
		}
		//char buffer[1024];
		//if(fgets(buffer, 1024, fp) != NULL)
		//{
			//int _numVertex, _LabelNum;
			//sscanf(buffer, "%d %d\n", &_numVertex, &_LabelNum);
			//init(_numVertex, _LabelNum);
		//}
		//for(int i = 0; i < numVertex; i ++)
		//{
			//if(fgets(buffer, 1024, fp) != NULL)
			//{
				//int len = strlen(buffer);
				//int pos = 0;
				//while(true)
				//{
					//if(pos >= len || buffer[pos] == '\n')
						//break;
					//else if(buffer[pos] == ' ')
						//pos ++;
					//else
					//{
						//int value = atoi(&(buffer[pos]));
						//vList[i].push_back(value);
						//while(true)
						//{
							//if(pos >= len || buffer[pos] == ' ' || buffer[pos] == '\n')
								//break;
							//else pos ++;
						//}
					//}
				//}
			//}
		//}

		//while(!feof(fp))
		//{
			//if(fgets(buffer, 1000, fp) != NULL)
			//{
				//int from, to;
				//sscanf(buffer, "%d %d\n", &from, &to);
				//addEdge(from, to);
			//}
		//}
		this->transform();
		return NativeParseStatus::Loaded;
	}
};

//寧몸써듐怜唐寧몸label
class Query
{
public:
	int LabelNum;
	int numVertex;

	int *vList;

	//vector <int> *labelList;

	vector <labelVlist> *graList;

	DGraph* real_graph;   //directed graph with edge labels

	Query()
	{
		vList = NULL;
		graList = NULL;
		real_graph = NULL;
	}

	~Query()
	{
		if(vList != NULL)
			delete []vList;
		//if(labelList != NULL)
		//	delete []labelList;
		if(graList != NULL)
			delete []graList;
		if(real_graph != NULL)
		{
			delete real_graph;
		}
	}

    void transform()
    {
        for(int i = 0; i < numVertex; ++i)
        {
            sort(graList[i].begin(), graList[i].end());
            for(std::size_t j = 0; j < graList[i].size(); ++j)
            {
                vector<int>& vl = graList[i][j].vlist;
                sort(vl.begin(), vl.end());
                vl.erase(unique(vl.begin(), vl.end()), vl.end());
            }
        }
    }

	void init(int _numVertex, int _LabelNum)
	{
		LabelNum = _LabelNum;
		numVertex = _numVertex;

		vList = new int [numVertex];

		//labelList = new vector <int> [LabelNum];

		graList = new vector <labelVlist> [numVertex];
	}

	void addVertex(int v, int l)
	{
		vList[v] = l;
		//labelList[l].push_back(v);
	}

	int contain(int e, vector <labelVlist> *temp)
	{
		int Size = temp->size();
		for(int i = 0; i < Size; i ++)
		{
			if((*temp)[i].label == e)
				return i;
		}
		return -1;
	}

	void addEdge(int from, int to)
	{
		int label = vList[to];
		int pos = contain(label, &(graList[from]));
		if(pos != -1)
		{
			graList[from][pos].vlist.push_back(to);
		}
		else
		{
			labelVlist l;
			l.label = label;
			l.vlist.push_back(to);
			graList[from].push_back(l);
		}

		label = vList[from];
		pos = contain(label, &(graList[to]));
		if(pos != -1)
		{
			graList[to][pos].vlist.push_back(from);
		}
		else
		{
			labelVlist l;
			l.label = label;
			l.vlist.push_back(from);
			graList[to].push_back(l);
		}
	}

	//NOTICE: maintain two kinds of structures, undirected(for TurboISO) and directed(for verification)
	//There are two createGraph() functions in this file, one for Query and one for Graph
	NativeParseStatus createGraph(FILE *fp)
	{
		NativeGraphRecord record;
		const NativeParseStatus status =
			readNativeGraph(fp, true, record, "query");
		if(status != NativeParseStatus::Loaded)
			return status;
		try
		{
			this->numVertex = record.vertex_count;
			this->LabelNum = record.vertex_label_count;
			vList = new int[numVertex];
			graList = new vector<labelVlist>[numVertex];
			this->real_graph = new DGraph;
			this->real_graph->vertices.resize(static_cast<std::size_t>(numVertex));
			for(int id = 0; id < numVertex; ++id)
			{
				const int label =
					record.vertex_labels[static_cast<std::size_t>(id)].front();
				addVertex(id, label);
				this->real_graph->vertices[static_cast<std::size_t>(id)].label = label;
			}
			for(const NativeEdgeRecord& edge : record.edges)
			{
				addEdge(edge.from, edge.to);
				this->real_graph->addEdge(edge.from, edge.to, edge.label);
			}
		}
		catch(const bad_alloc&)
		{
			cerr << "Error: cannot allocate query graph.\n";
			return NativeParseStatus::Error;
		}
		//char buffer[1024];
		//if(fgets(buffer, 1024, fp) != NULL)
		//{
			//int _numVertex, _LabelNum;
			//sscanf(buffer, "%d %d\n", &_numVertex, &_LabelNum);
			//init(_numVertex, _LabelNum);
		//}
		//for(int i = 0; i < numVertex; i ++)
		//{
			//if(fgets(buffer, 1024, fp) != NULL)
			//{
				//vList[i] = atoi(buffer);
			//}
		//}

		//while(!feof(fp))
		//{
			//if(fgets(buffer, 1000, fp) != NULL)
			//{
				//int from, to;
				//sscanf(buffer, "%d %d\n", &from, &to);
				//addEdge(from, to);
			//}
		//}
		this->transform();
		return NativeParseStatus::Loaded;
	}
};

class NECTree
{
public:
	int numVertex;
	vector <int> vList;				//the label of NECTree node
	vector < vector <int> > NEC;		//the real query nertices of a NECTree node
	vector <int> parent;			//parent node
	vector <Child> child;			//child nodes

	void init()
	{
		numVertex = 0;
	}
};

class CRTree
{
public:
	vector <vector <int> > *CR;	 //the real data vertices of CRTree
	vector <int> *parent;			//parent nodes: each query node u has a set of parent nodes v (data nodes)   CR(uprime,v) of vprimes

	void init(int num)
	{
		CR = new vector <vector <int> > [num];
		parent = new vector <int> [num];
	}

	~CRTree()
	{
		if(CR != NULL)
			delete []CR;
		if(parent != NULL)
			delete []parent;
	}
};

string int2string(long n)
{
    string s;
    stringstream ss;
    ss<<n;
    ss>>s;
    return s;
}

//NOTICE: there does not exist itoa() function in Linux, atoi() is included in stdlib.h
//itoa() is not a standard C function, and it is only used in Windows.
//However, there do exist a function called sprintf() in standard library which can replace itoa()
//char str[255];
//sprintf(str, "%x", 100); //change 100 to 16-base string
char* itoa(int num, char* str, int radix) //the last parameter means the number's radix: decimal, or octal formats
{
	//index table
	char index[]="0123456789ABCDEF";
	unsigned unum;
	int i=0,j,k;
	if(radix==10&&num<0)  //negative in decimal
	{
		unum=(unsigned)-num;
		str[i++]='-';
	}
	else unum=(unsigned)num;
	do{
		str[i++]=index[unum%(unsigned)radix];
		unum/=radix;
	}while(unum);
	str[i]='\0';
	//reverse order
	if(str[0]=='-')k=1;
	else k=0;
	char temp;
	for(j=k;j<=(i-1)/2;j++)
	{
		temp = str[j];
		str[j] = str[i-1+k-j];
		str[i-1+k-j] = temp;
	}
	return str;
}

long get_cur_time()
{
    timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec*1000 + tv.tv_usec/1000);
}

void process_mem_usage(double& vm_usage, double& resident_set) {
    vm_usage = 0.0;
    resident_set = 0.0;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        vm_usage = static_cast<double>(counters.PrivateUsage) / 1024.0;
        resident_set = static_cast<double>(counters.WorkingSetSize) / 1024.0;
    }
#elif defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    unsigned long virtual_pages = 0;
    unsigned long resident_pages = 0;
    if (statm >> virtual_pages >> resident_pages) {
        const long page_size = sysconf(_SC_PAGESIZE);
        if (page_size > 0) {
            vm_usage = static_cast<double>(virtual_pages) * page_size / 1024.0;
            resident_set = static_cast<double>(resident_pages) * page_size / 1024.0;
        }
    }
#endif
}

// void myTimeout(int signo) {
//     switch(signo) {
//         case SIGALRM:
//             printf("This query runs time out!\n");
//             exit(1);
//         default:
//             break;
//     }
// }

// void timeLimit(int seconds) {
//     struct itimerval tick;
//     //signal(SIGALRM, exit);
//     signal(SIGALRM, myTimeout);
//     memset(&tick, 0, sizeof(tick));
//     //Timeout to run first time
//     tick.it_value.tv_sec = seconds;
//     tick.it_value.tv_usec = 0;
//     //After first, the Interval time for clock
//     tick.it_interval.tv_sec = seconds;
//     tick.it_interval.tv_usec = 0;
//     if(setitimer(ITIMER_REAL, &tick, NULL) < 0)
//         printf("Set timer failed!\n");
// }


// void noTimeLimit() {
//     struct itimerval tick;
//     memset(&tick, 0, sizeof(tick));
//     if(setitimer(ITIMER_REAL, &tick, NULL) < 0)
//         printf("Withdraw timer failed!\n");
// }



// Cooperative timeout. The previous Windows implementation blocked before
// matching and its completion callback could not run from a non-alertable wait.
bool query_timeout_enabled = false;
std::chrono::steady_clock::time_point query_deadline;

void timeLimit(int seconds)
{
    query_timeout_enabled = seconds > 0;
    if (query_timeout_enabled) {
        query_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    }
}

void noTimeLimit()
{
    query_timeout_enabled = false;
}

bool timeLimitExceeded()
{
    return query_timeout_enabled && std::chrono::steady_clock::now() >= query_deadline;
}

#endif
