# `ours_gopher` / `ours_gopher_gpu` 구현 설계

## 1. 범위와 근거

이 문서는 다음 자료를 함께 읽고, 현재 저장소의 `ours`를 기준 구현으로 삼아 작성한 구현 인계 문서다.

- `appendix/260408_cdh.pdf`
- `appendix/Durable subgraph matching on temporal graphs(2022).pdf`
- `appendix/Gopher-Efficient Dynamic Graph Pattern Mining via DAG-Driven Execution(2026).pdf`
- `appendix/논문 방법론 기여 설명.pdf`
- 현재 `ours/` 및 참고용 `Ours_DFS/`

목표는 Gopher 전체를 복제하는 것이 아니다. `ours`의 오프라인 consecutive-durable matching 의미를
유지하면서, 후보 탐색 중 반복되는 구조 집합 및 시간 집합 연산을 Gopher식 DAG와 공통 부분식
재사용으로 줄이는 것이다.

구현 전제는 다음과 같다.

1. `ours`와 `original`의 정확성 감사 및 수정이 먼저 끝나야 한다.
2. `ours_gopher`는 수정 완료된 `ours`와 동일한 입력 의미와 결과를 내야 한다.
3. 최적화가 켜졌을 때와 꺼졌을 때 최종 매칭 집합은 완전히 같아야 한다.
4. CPU 단일 스레드가 정확성 기준이며, 병렬 CPU와 ROCm은 동일 알고리즘의 실행 백엔드다.

### 1.1 원문 근거 추적

| 설계 항목 | 원문 근거 |
|---|---|
| TD-tree DFS query decomposition과 selectivity | TD-Tree §4, PDF 4–5쪽 |
| TD-tree block, growth, non-tree filter, trim | TD-Tree §5, PDF 5–8쪽 |
| parent-before-child enumeration과 pushed-down duration test | TD-Tree §6–7, PDF 8–10쪽 |
| consecutive-pair graph filter | `260408_cdh.pdf` §3.1, PDF 3–4쪽 |
| temporal-aware query order와 exact consecutive verification | `260408_cdh.pdf` §3.2–3.3, PDF 4–5쪽 |
| Gopher mapping analysis, schedule, DAG construction/merge | Gopher §4, PDF 5–7쪽 |
| substructure tasking, skew split, restoration | Gopher §5, PDF 8–9쪽 |
| Gopher ablation, compile overhead, labels, CPU scaling | Gopher §7.3–7.7, PDF 11–13쪽 |
| dependency-equivalent partial mapping과 2단계 key | 방법론 설명 PDF 15–18, 37–48쪽 |
| branch-invariant `B`/`S`, monotonic pruning | 방법론 설명 PDF 18–21, 33–44쪽 |
| fixed reuse-aware order, 통계, ablation | 방법론 설명 PDF 22–29, 49–57쪽 |

## 2. 논문별 핵심과 통합 시 경계

### 2.1 TD-Tree

TD-Tree 논문의 핵심은 다음과 같다.

- 모든 스냅샷을 합친 그래프에서 간선별 활성 시간 집합을 저장한다.
- 질의를 DFS spanning tree와 non-tree edges로 분해한다.
- 부모 후보별 자식 후보 관계를 TD-tree block으로 저장한다.
- TD-tree를 위·아래 방향으로 trim한다.
- parent-before-child 순서로 injective mapping을 열거하며 non-tree edge와 시간 조건을 검사한다.

원 TD-Tree 논문은 `|I| >= k`, 즉 공통 활성 시점의 개수로 durability를 정의한다. 반면
`260408_cdh.pdf`와 현재 `ours`는 공통 활성 구간에 길이 `k` 이상의 **연속 구간**이 있어야 한다.
따라서 `ours_gopher`는 원 논문의 단순 `|TS| >= k` 검사를 가져오면 안 되고, 반드시
`maxConsecutiveRun(I) >= k` 의미를 보존해야 한다.

### 2.2 `260408_cdh.pdf`

제안 방법은 TD-Tree 위에 다음 두 요소를 더한다.

- 스냅샷 병합 때 두 연속 스냅샷에 한 번도 나타나지 않은 간선을 제거한다.
- 시간 특성을 반영한 선택률로 DFS root와 순서를 정한다.

현재 `ours`는 방향성 간선, 정렬·압축된 interval list, 정확한 tree/non-tree edge 검사, injectivity,
확장 단계별 공통 interval 교집합을 구현한다. `ours_gopher`는 이 의미를 그대로 사용한다.

주의할 점이 하나 있다. `260408_cdh.pdf`의 식 (3.1), 식 (3.2), `arg min`, 그리고
“희귀하고 구조적으로 강한 정점을 선택한다”는 설명은 서로 수학적으로 일관되지 않는다.
현재 `ours`는 PDF에 인쇄된 식

```text
S_temp(u) = deg_Q(u) / (|V_L(u)| * A_dur(u)), root = arg min
```

을 그대로 따른다. Gopher 통합에서 이 문제를 조용히 바꾸지 않는다. 재현용 `paper` 순서와 새로운
`reuse` 순서를 별도 모드로 두고 ablation해야 한다.

### 2.3 Gopher

Gopher는 업데이트 간선을 패턴의 여러 위치에 놓아 만든 mapping instances가 같은 이웃 집합
교집합을 반복한다는 점을 이용한다. 각 instance의 set program을 DAG로 만들고, canonical
fingerprint가 같은 부분식을 병합하며, 공유 부분구조를 태스크 단위로 병렬 실행한다.

논문에서 중요한 구성은 다음과 같다.

- edge/vertex symmetry를 고려한 mapping instance 생성
- 비용과 공통 부분구조 재사용을 함께 고려한 schedule
- neighborhood lookup, set intersection, membership test 등의 DAG IR
- available-expression list를 이용한 instance 내부 CSE
- canonical path fingerprint와 깊이 우선 병합을 이용한 instance 간 CSE
- 부분구조 단위 병렬성, work stealing, 고차수 태스크 분할
- mapping instance flag와 mapping table을 이용한 embedding restoration

그러나 Gopher의 주요 공유 단위인 “하나의 update edge에서 나온 여러 mapping instances”는
오프라인 durable matching에 존재하지 않는다. 서로 다른 root 후보는 보통 서로 다른 데이터 정점에
바인딩되므로 DAG 모양이 같다는 이유만으로 결과를 공유할 수 없다.

따라서 다음과 같이 바꾼다.

```text
Gopher:
  같은 update edge를 공유하는 mapping instances 사이의 공통 부분식

ours_gopher:
  연산이 실제로 의존하는 데이터 바인딩이 같은 partial mappings 사이의 공통 부분식
```

이를 dependency-equivalent partial mappings라 부른다.

### 2.4 `Ours_DFS`를 구현 기반으로 사용하지 않는 이유

`Ours_DFS`는 아이디어 참고용으로만 사용해야 한다.

- 이름과 달리 query decomposition, TD-tree traversal, 최종 frontier가 BFS 중심이다. TD-Tree와
  `260408_cdh.pdf`가 요구하는 DFS 기반 긴 경로 pruning을 재현하지 않는다.
- `main.cpp`가 Windows `windows.h`/`psapi.h`에 고정되어 현재 Linux 환경에서 그대로 빌드되지 않는다.
- GPU backend는 CUDA 전용이고 `nvcc`/Visual Studio 경로를 전제로 하므로 ROCm 대상이 아니다.
- GPU 후보 호출마다 CSR과 모든 vertex statistics를 다시 할당·복사해 작은 확장 연산의 전송 비용이
  매우 크다.
- GPU 할당/복사/kernel 실패 시 동일 요청을 CPU로 재실행하지 않고 빈 결과를 반환할 수 있어,
  runtime failure가 false negative로 바뀔 수 있다.
- `distinct_neighbor_label_count >= required_degree`는 같은 라벨의 query neighbor가 여러 개인 경우
  안전하지 않다. 예를 들어 같은 라벨 자식 둘을 만족하는 데이터 정점은 distinct label 수가 1이어도
  유효할 수 있다.
- root/child의 `required_degree`가 전체 directed query degree가 아니라 spanning-tree child 수를
  사용하고, query/data 방향 통계를 분리하지 않는다.
- 현재 CPU/GPU filter는 Gopher의 공통 부분식 DAG, dependency-binding key, bounded memo,
  interval-support 공유를 구현하지 않는다.

따라서 `ours_gopher`는 감사 완료된 `ours`의 directed interval/TD-tree/oracle 구조를 복제한 뒤,
`Ours_DFS`에서는 “backend interface를 분리한다”는 발상만 참고한다.

## 3. 보존해야 할 외부 의미

`ours_gopher`의 기본 실행은 다음 기존 호출과 호환되어야 한다.

```text
td_tree_gopher DATA QUERY K
td_tree_gopher DATA QUERY K LABEL_SEED
```

기본 의미는 다음과 같다.

- 입력의 `u v t`는 directed temporal edge `u -> v`다.
- 질의의 `A B` 또는 `id:A id2:B`도 directed query edge다.
- 데이터 외부 정점 ID는 내부에서 compact ID로 바꾸고 출력 때 복원한다.
- 데이터 라벨은 `A`–`E`, 기본 seed `42`로 결정적이다.
- 간선 interval은 정렬되고 서로 겹치지 않는 닫힌 구간이다.
- 전처리 필터 때문에 `k >= 2`만 허용한다.
- 최종 결과는 모든 질의 간선의 공통 활성 interval에 길이 `k` 이상의 연속 구간이 있을 때만
  출력한다.
- 출력의 query vertex 순서는 query ID 순서다.

최적화는 결과 순서가 아니라 결과 **집합**을 보존해야 한다. 작은 검증 입력에서는 정렬된 전체
결과를 비교하고, 큰 성능 실험에서는 count와 순서 독립 result digest를 함께 비교한다.

## 4. 제안 아키텍처

전체 파이프라인은 다음과 같다.

```text
read/filter temporal graph
        |
read query
        |
paper/reuse-aware fixed query plan
        |
TD-tree candidate relation build/trim
        |
compile Parameterized Temporal Candidate DAG
        |
fixed-order durable DFS
  |-- evaluate structural candidate DAG
  |-- apply branch-specific injectivity/symmetry filters
  |-- evaluate candidate temporal-support DAG
  |-- monotonic consecutive-duration pruning
  `-- recurse
        |
matches + exact statistics + result digest
```

핵심 원칙은 “비싼 branch-invariant 연산만 공유하고 branch-specific 상태는 공유하지 않는다”다.

## 5. 수학적 정의

고정된 query schedule을 `π`라고 하고, 현재 partial mapping을 `φ`라고 하자. 다음 query vertex를
`u`라고 한다. `Predπ(u)`는 `u`와 인접하며 schedule상 이미 매핑된 query vertices다.

### 5.1 구조 기본 후보

방향을 보존한 기본 후보 집합은 다음과 같다.

```text
B(u, φ) =
    C0(u)
    ∩  intersection Out(φ(x)) for every query edge x -> u
    ∩  intersection In (φ(x)) for every query edge u -> x
```

여기서 `C0(u)`는 label, out/in degree, directed NLF, vertex-duration 필요조건을 통과한 후보다.
실제 구현에서는 TD-tree의 `block(u, φ(parent(u)))`가 `C0`와 tree-edge 조건을 이미 포함하므로
이를 첫 입력으로 사용하고, 이미 매핑된 나머지 query neighbors의 adjacency와 교집합한다.

`B(u, φ)`에는 다음을 넣지 않는다.

- 현재 branch에서 이미 사용한 데이터 정점 제거
- branch별 symmetry order
- 현재 active support

이 조건들은 캐시 hit 후 각 DFS branch에서 적용한다.

### 5.2 새 간선의 시간 지원

후보 `v`로 `u`를 확장할 때 새로 닫히는 모든 query edge의 공통 시간 지원을 다음과 같이 둔다.

```text
S(u -> v | φ) =
    intersection TS(φ(x), v) for every query edge x -> u
    intersection TS(v, φ(x)) for every query edge u -> x
```

reciprocal query arcs가 있으면 두 방향의 temporal edge interval을 모두 포함한다.

현재 partial mapping이 만족하는 공통 활성 interval을 `I(φ)`라고 하면

```text
I(φ ∪ {u -> v}) = I(φ) ∩ S(u -> v | φ)
```

이다. 공유하기 좋은 값은 `S`이며, `I(φ) ∩ S` 전체가 아니다. `I(φ)`는 이전에 닫힌 모든 query
edge에 의존하므로 branch마다 다르다.

### 5.3 안전한 조기 pruning

interval 교집합은 interval을 늘릴 수 없다. 따라서 다음 두 pruning은 완전성을 보존한다.

```text
maxConsecutiveRun(S) < k
    => candidate v 제거

maxConsecutiveRun(I(φ) ∩ S) < k
    => 해당 DFS branch 제거
```

모든 중간 interval 교집합에서 길이 `k` 미만인 interval 조각을 버려도 안전하다. 이후 교집합으로
그 조각이 다시 길어질 수 없기 때문이다.

## 6. DAG IR

### 6.1 노드 종류

최소 CPU 구현은 다음 연산을 지원한다.

```cpp
enum class DagOp : std::uint8_t {
    TdBlockLookup,
    OutNeighborLookup,
    InNeighborLookup,
    StructuralIntersection,
    EdgeTimeLookup,
    TemporalIntersection,
    LongestConsecutiveRun
};

enum class DagResultKind : std::uint8_t {
    CandidateView,
    CandidateSet,
    IntervalView,
    IntervalSet,
    Integer
};
```

`LabelFilter`, `DegreeFilter`, directed NLF는 TD-tree 초기 후보 생성에 이미 들어 있으므로 v1 DAG에서
중복하지 않는다. 나중에 TD-tree와 planner를 완전히 통합할 경우 독립 노드로 승격할 수 있다.

### 6.2 노드 정의

```cpp
using QueryVertex = int;
using DataVertex = int;
using NodeId = std::uint32_t;

struct DagNode {
    NodeId id;
    DagOp op;
    DagResultKind result_kind;
    std::vector<NodeId> parents;

    // 오름차순 query vertex ID. 이 노드 결과에 필요한 최소 바인딩 집합.
    std::vector<QueryVertex> dependencies;

    // target qid, query edge direction/id, label 등 정적 매개변수.
    NodeParameter parameter;

    // canonical expression 전체와 그 hash를 모두 보관한다.
    std::string canonical_expression;
    std::uint64_t static_fingerprint;
    std::uint32_t depth;
};
```

### 6.3 static fingerprint

fingerprint는 데이터 정점 ID가 아니라 질의 연산의 의미를 표현한다.

```text
FP(n) = H(
  operator,
  result kind,
  query role / edge direction / constraint signature,
  canonical parent fingerprints
)
```

교집합처럼 교환법칙이 있는 연산은 parent fingerprint와 canonical expression을 정렬한다.
`Intersect(X,Y)`와 `Intersect(Y,X)`는 같은 fingerprint가 되어야 한다. 방향이 다른 adjacency 또는
temporal edge lookup은 절대로 합치지 않는다.

hash가 같은 것만으로 노드를 합치지 않는다. hash bucket 안에서 canonical expression 전체를 다시
비교한다. 깊은 공통 표현식부터 병합하는 Gopher의 원칙을 따르되, 한 query 안에서는 hash-consing
중 즉시 동일 노드를 재사용해도 같은 결과를 얻는다.

### 6.4 runtime key

static fingerprint가 같아도 실제 데이터 바인딩이 다르면 결과가 다르다. canonical node가 만들어진
후 runtime memo key는 다음과 같다.

```text
RuntimeKey(n, φ) =
  (canonicalNodeId,
   φ(Dep(n)) in ascending query-vertex order,
   executionContextId)
```

temporal support 노드에서는 target `u`의 임시 바인딩 `u -> v`도 dependency에 포함한다.

```cpp
struct RuntimeKey {
    NodeId node;
    std::vector<DataVertex> bindings;
    std::uint64_t execution_context_id;

    bool operator==(const RuntimeKey&) const;
};
```

`execution_context_id`는 graph epoch, temporal window, `k`, query plan을 묶은 실행 단위 ID다.
현재 그래프가 immutable이고 memo가 한 query 실행에만 존재하면 별도 필드를 반복 저장하지 않아도
되지만, API 의미상 컨텍스트가 다르면 결과를 재사용하지 않는다는 불변식은 유지한다.

전체 partial mapping을 key에 넣지 않는다. 노드의 최소 dependency만 넣어야 서로 다른 branch의
실제 중복을 발견할 수 있다.

## 7. 코드 수준 API

### 7.1 TD-tree 읽기 API

`TDTree`는 matcher가 내부 배열을 수정하지 않고 읽을 수 있도록 다음 인터페이스를 제공한다.

```cpp
class TDTree {
public:
    const std::vector<int>& rootCandidates() const;

    // 없으면 nullptr. 반환 vector는 TDTree 생존 동안 immutable.
    const std::vector<int>* candidatesForParent(
        QueryVertex query_vertex,
        DataVertex parent_data_vertex) const;
};
```

### 7.2 query plan

```cpp
enum class OrderMode {
    Paper,
    TdTreeOriginal,
    ReuseAware
};

struct QueryPlan {
    QueryVertex root;
    std::vector<QueryVertex> order;
    std::vector<QueryVertex> parent;
    std::vector<std::vector<QueryVertex>> earlier_neighbors;
    std::vector<std::pair<QueryVertex, QueryVertex>> non_tree_edges;
    std::uint64_t plan_id;
};
```

모든 plan은 다음을 만족해야 한다.

- query graph의 모든 정점을 정확히 한 번 포함한다.
- root가 첫 번째다.
- root가 아닌 정점의 TD-tree parent가 자신보다 먼저 나온다.
- 한 실행 동안 plan은 고정되고 결정적이다.

초기 구현에서는 DFS 중 동적으로 다음 정점을 바꾸지 않는다. 동적 순서는 계획 비용, 중복 결과
위험, cache-key 불안정성을 동시에 키운다.

### 7.3 compiler

```cpp
class CandidateDagCompiler {
public:
    CandidateDag compile(
        const Graph& query,
        const QueryPlan& plan,
        const QueryDecomposition& decomposition) const;
};
```

각 `u`에 대해 compiler는 다음 두 root node를 기록한다.

```cpp
struct CandidateDag {
    std::vector<DagNode> nodes;
    std::vector<NodeId> structural_root_by_query_vertex;
    std::vector<NodeId> temporal_root_by_query_vertex;
    std::vector<NodeId> temporal_longest_run_by_query_vertex;
};
```

### 7.4 executor

```cpp
struct TemporalSupport {
    std::vector<TimeInterval> intervals;
    int longest_run = 0;
};

using DagValue = std::variant<
    CandidateView,
    std::shared_ptr<const std::vector<int>>,
    IntervalView,
    std::shared_ptr<const std::vector<TimeInterval>>,
    int>;

class DagExecutor {
public:
    std::shared_ptr<const std::vector<int>> structuralCandidates(
        QueryVertex u,
        const SearchState& state);

    std::shared_ptr<const TemporalSupport> temporalSupport(
        QueryVertex u,
        DataVertex v,
        const SearchState& state);
};
```

graph adjacency와 TD block은 immutable view로 반환하고 memo에 복사하지 않는다. 실제 교집합 결과만
owned immutable value로 저장한다.

### 7.5 search state

```cpp
struct SearchState {
    std::vector<DataVertex> mapping;       // -1 if unmapped
    std::vector<std::uint8_t> used;        // branch-specific, never memoized
    std::vector<TimeInterval> active;      // I(phi), branch-specific
    bool has_active_support = false;
    std::size_t depth = 0;
};
```

### 7.6 matcher

```cpp
class GopherMatcher {
public:
    MatchSummary run(std::ostream* match_output);

private:
    void explore(SearchState& state, ThreadContext& thread);
};
```

핵심 DFS는 다음 순서를 고정한다.

```text
u = plan.order[state.depth]
B = executor.structuralCandidates(u, state)

for v in B:
    generatedCandidates++
    if used[v]: continue
    apply branch-specific symmetry checks

    S = executor.temporalSupport(u, v, state)
    if S.longest_run < k: prune candidate

    next = state.has_active_support
         ? intersect(state.active, S.intervals, minimum_length=k)
         : S.intervals
    if next is empty: prune branch

    assign u -> v
    state.active = next
    recurse
    restore state
```

`B`가 모든 earlier neighbor adjacency를 정확히 교차했다면 별도 non-tree structural edge 검사는
불필요하다. 초기 안전 구현에서는 legacy exact check를 한 번 더 실행할 수 있고, 검증이 끝난 뒤
`--verify-dag`에서만 중복 검사한다.

## 8. bounded memo cache

무제한 캐시는 대형 데이터에서 TD-tree 절감 효과를 상쇄할 수 있다. 기본 구현은 byte budget을
넘기지 않는 sharded memo를 사용한다.

```cpp
struct MemoOptions {
    std::size_t budget_bytes = 256ULL << 20;    // 256 MiB
    std::size_t max_entry_bytes = 8ULL << 20;  // 8 MiB
    std::size_t shard_count = 64;
    std::size_t minimum_estimated_work = 128;
    enum class Admission { All, CostAware, SecondTouch };
    Admission admission = Admission::CostAware;
};
```

권장 v1 정책은 다음과 같다.

1. key hash로 shard를 정한다.
2. hit value는 immutable `shared_ptr<const T>`로 반환한다.
3. miss 계산은 cache lock 밖에서 수행한다.
4. 같은 key의 동시 miss는 single-flight entry로 한 번만 계산하거나, 초기 단순 구현에서는 중복
   계산 후 첫 insert만 채택한다. 통계에 duplicate miss를 별도 기록한다.
5. 결과가 `max_entry_bytes`보다 크면 저장하지 않는다.
6. 예상 입력 work가 작고 두 번째 호출도 아닌 entry는 저장하지 않는다.
7. 새 entry로 budget을 넘으면 저장하지 않고 결과만 호출자에게 돌려준다.
8. v1은 실행 중 eviction을 하지 않아 참조 안정성과 재현성을 우선한다. 필요할 때만 shard-local
   LRU를 추가한다.

구조·시간 cache를 별도 budget으로 나누거나, 공통 budget에서 result type별 사용량을 기록한다.

다음 값은 캐시하면 안 된다.

- `B - usedVertices`
- `I(φ) ∩ S`
- branch별 최종 boolean
- output match

## 9. CPU 병렬 실행

정확성 기준은 `--threads 1`이다. 병렬 모드는 동일 fixed plan과 DAG executor를 사용한다.

### 9.1 태스크 경계

초기 태스크는 root candidate별로 만든다. root binding이 다르면 최종 mapping 집합도 겹치지 않으므로
안전하다. 한 root가 매우 큰 경우 지정한 split depth 또는 큰 candidate frontier에서 SearchState를
복제해 하위 태스크로 나눈다.

```cpp
struct SearchTask {
    SearchState state;
    std::uint64_t deterministic_prefix_id;
};
```

### 9.2 스케줄러

- worker별 deque와 work stealing을 사용한다.
- graph, TD-tree, query plan, DAG는 모두 immutable이다.
- memo cache는 sharded shared cache다.
- 통계는 thread-local로 모은 뒤 종료 시 합친다.
- `--count-only`는 atomic/global reduction 대신 thread-local count와 digest를 마지막에 합친다.
- 전체 매칭을 파일로 쓸 때 기본은 single-thread다. 병렬 출력이 필요하면 prefix task별 임시
  buffer/file을 만들고 `deterministic_prefix_id` 순으로 합친다.

Gopher처럼 부분구조 DAG node 자체를 매번 독립 태스크로 만드는 것은 v1에서 피한다. 작은 query의
짧은 집합 연산은 task overhead가 더 클 수 있다. root/frontier search task와 shared DAG memo만으로
먼저 이득을 검증한다.

## 10. 구현된 v1 static reuse-aware schedule

평가에 사용한 구현은 두 order만 분리한다.

- `paper`: 현재 `ours`의 식과 DFS 순서를 그대로 재현한다.
- `reuse`: TD relation 크기, 이미 앞에서 검사할 수 있는 제약 수, 구조적 reuse potential을
  사용해 parent-before-child 순서를 질의 실행 전에 한 번 결정한다.

두 순서 모두 검색 중에는 바뀌지 않는다. 따라서 모든 스레드가 같은 plan과 안정적인 runtime
key를 사용한다. 현재 `reuse`는 **정적인 재사용 인지 휴리스틱**이며, 시간 생존율을 직접
추정하거나 cache hit를 관찰해 온라인으로 다음 정점을 바꾸지 않는다.

아래 식과 beam/look-ahead 절차는 후속 버전의 설계 후보이지 이번 구현이나 성능평가의
기능이 아니다.

```text
futureScore(u) =
  estimatedCandidateCount(u)
  * estimatedTemporalSurvival(u)
  * estimatedOperatorCost(u)
  / (1 + lambda * estimatedReuseProbability(u))
```

이를 구현하려면 deterministic sampling, plan-selection 비용 계측, beam search, temporal union
upper bound, cache-hit 기반 통계를 별도 기능으로 추가하고 각각 ablation해야 한다. 따라서 본
보고서와 `ours_gopher` README는 이 기능들을 구현했다고 주장하지 않는다.

## 11. 디렉터리와 파일 설계

### 11.1 `ours_gopher/`

```text
ours_gopher/
  README.md
  CMakeLists.txt
  build.sh
  build.ps1
  run_tests.sh
  run_tests.ps1
  run_unique_filtered.ps1

  main.cpp
  Utils.h
  Utils.cpp
  query_decomposition.h
  query_decomposition.cpp
  TDTree.h
  TDTree.cpp

  QueryPlan.h
  QueryPlan.cpp
  CandidateDag.h
  CandidateDag.cpp
  DagExecutor.h
  DagExecutor.cpp
  BoundedMemo.h
  BoundedMemo.cpp
  GopherMatcher.h
  GopherMatcher.cpp
  SearchStatistics.h
  SearchStatistics.cpp
  ResultDigest.h
  ResultDigest.cpp

  tests/
    test_time_intervals.cpp
    test_candidate_dag.cpp
    test_runtime_keys.cpp
    test_cache_budget.cpp
    test_gopher_oracle.cpp
    test_parallel_parity.cpp
```

`Utils`, query decomposition, TD-tree를 `../ours`에서 소스 include하지 않는다. 감사 완료된 버전을
복사해 독립 실행 파일로 만들고, parity test로 drift를 잡는다.

### 11.2 `ours_gopher_gpu/`

GPU 폴더는 CPU 코어를 다시 복제하지 않고 `../ours_gopher`를 정적 라이브러리로 링크한다.

```text
ours_gopher_gpu/
  README.md
  CMakeLists.txt
  build_rocm.sh
  HipDagBackend.h
  HipDagBackend.hip
  HipGraphStore.h
  HipGraphStore.hip
  HipBatchScheduler.h
  HipBatchScheduler.cpp
  main.cpp
  tests/
    test_hip_cpu_parity.cpp
    test_hip_interval_parity.cpp
```

이 구조가 CPU/GPU 의미 차이와 코드 drift를 가장 적게 만든다.

## 12. CLI와 출력

권장 CLI는 기존 positional arguments를 유지하고 뒤에 옵션을 추가한다.

```text
td_tree_gopher DATA QUERY K [LABEL_SEED]
  [--engine legacy|gopher]
  [--order paper|tdtree|reuse]
  [--memo none|structural|temporal|dag]
  [--cache-mb N]
  [--threads N]
  [--count-only]
  [--verify-dag]
  [--metrics FILE.json]
  [--output-dir DIR]
```

기본값은 정확한 비교를 위해 다음처럼 둔다.

```text
engine=gopher
order=paper
memo=dag
cache-mb=256
threads=1
label-seed=42
```

GPU 실행 파일은 같은 옵션에 다음을 더한다.

```text
  [--device auto|cpu|gpu]
  [--gpu-batch N]
  [--gpu-min-work N]
  [--gpu-streams N]
```

기존 호환 파일은 유지한다.

- `matching_results_<dataset>.txt`
- `timing_results_<dataset>.txt`

추가로 machine-readable JSON을 쓴다. Excel 생성은 이 JSON들을 읽어 수행한다.

```json
{
  "method": "ours_gopher",
  "dataset": "...",
  "query": "...",
  "k": 3,
  "seed": 42,
  "threads": 1,
  "device": "cpu",
  "order": "paper",
  "memo": "dag",
  "result_count": 0,
  "result_digest": "...",
  "timings_ms": {},
  "search": {},
  "cache": {},
  "memory_bytes": {}
}
```

## 13. 필수 통계

### 13.1 논리 탐색량

- root candidates
- DFS states / recursive calls
- generated candidates
- completed structural mappings
- final durable matches
- pruned by injectivity
- pruned by structural intersection
- pruned by `maxRun(S) < k`
- pruned by `I ∩ S`

### 13.2 실제 연산량

- logical / physical out-neighbor lookups
- logical / physical in-neighbor lookups
- logical / physical structural intersections
- logical / physical temporal edge lookups
- logical / physical temporal intersections
- logical / physical longest-run computations
- 각 연산 입력 원소 수 합계
- 비용 가중 saved work

### 13.3 DAG와 cache

- static nodes before/after hash-consing
- canonical-expression merge count
- structural/temporal/DAG memo hits, misses, bypasses
- duplicate concurrent misses
- hit entry 평균/최대 result size
- cache lookup time
- cache compute time
- admitted/rejected bytes
- peak cache bytes
- budget rejections

단순 hit ratio만 보지 말고 다음 비용 가중 비율도 기록한다.

```text
weightedReuse =
  1 - physical estimated work / logical estimated work
```

### 13.4 병렬/GPU

- 생성/실행/steal task 수
- worker별 busy time과 imbalance
- queue 최대 길이
- GPU batch 수와 평균/최대 batch size
- CPU fallback request 수
- H2D, kernel, D2H, device compaction 시간
- host/device peak memory

## 14. 정확성 불변식

구현과 review에서 다음 항목을 assertion 또는 test로 증명해야 한다.

1. 모든 temporal interval list는 정렬되고, 겹치지 않으며, `start <= end`다.
2. directed reciprocal edges는 서로 다른 interval history를 가진다.
3. query plan은 연결된 query의 모든 정점을 한 번 포함하고 parent-before-child다.
4. 각 directed query edge는 나중에 매핑되는 endpoint의 temporal support에 정확히 한 번 들어간다.
5. `B(u,φ)`는 현재 상태에서 구조적으로 가능한 모든 후보를 포함한다.
6. injectivity와 branch symmetry는 cache 밖에서 적용한다.
7. `S(u->v|φ)`는 이번 확장에서 새로 닫히는 모든 directed query edge interval의 정확한 교집합이다.
8. `I(φ)`는 현재까지 양 endpoint가 매핑된 모든 query edge interval의 정확한 교집합이다.
9. 빈 interval 또는 `maxRun < k` pruning은 interval 교집합 단조성으로 안전하다.
10. static DAG merge는 hash뿐 아니라 canonical expression equality를 확인한다.
11. runtime memo equality는 canonical node와 모든 최소 dependency binding을 정확히 비교한다.
12. memo value는 immutable이고 graph/query/plan context를 넘어서 재사용하지 않는다.
13. fixed deterministic schedule과 injectivity 때문에 각 query-to-data mapping은 한 번만 생성된다.
14. threads/device/memo/order를 바꿔도 최종 매칭 집합은 brute-force oracle 및 `ours`와 같다.

## 15. 테스트 계획

### 15.1 단위 테스트

- interval intersection: 겹침, 접점, 포함, 빈 결과, 여러 조각
- `{1,3,5}`는 `k=3` durable이 아니며 `{2,3,4}`는 durable
- reciprocal directed edge interval 분리
- TD block lookup과 sorted candidate intersection
- `Out(φ(x))`와 `In(φ(x))` 방향 구분
- reciprocal query arc가 두 temporal edge를 모두 요구
- commutative intersection fingerprint canonicalization
- 방향/edge-role이 다른 노드는 merge하지 않음
- 인위적으로 같은 hash를 주어도 canonical expression이 다르면 merge하지 않음
- irrelevant binding만 다른 partial mappings는 같은 key
- relevant binding 또는 candidate가 다르면 다른 key
- 같은 base candidate cache 결과에 서로 다른 used set을 적용하면 branch 결과가 올바르게 다름
- cache budget, max entry, admission, bypass

### 15.2 oracle 테스트

작은 directed temporal graph를 생성해 모든 injective mappings를 brute force로 열거한다.

- query: path, star, triangle, cycle, house, dense directed pattern
- repeated labels와 `id:label` query vertices
- `k = 2,3,4,5`
- 여러 label seeds
- fragmented/non-fragmented interval
- sparse external IDs

각 경우 다음 결과 집합을 정확히 비교한다.

```text
brute force
ours
ours_gopher --memo none
ours_gopher --memo dag
ours_gopher --order reuse
ours_gopher --threads 1,2,4
ours_gopher_gpu --device cpu,gpu
```

### 15.3 중간 결과 shadow verification

`--verify-dag`에서는 legacy 계산과 DAG 계산을 동시에 실행해 다음을 매 node/확장마다 비교한다.

- structural candidate vector
- temporal support interval vector
- longest run
- next active support

최종 count만 비교하면 서로 다른 중간 오류가 우연히 상쇄될 수 있으므로 충분하지 않다.

### 15.4 동시성 도구

- ThreadSanitizer로 shared memo와 scheduler 검사
- AddressSanitizer/UBSan
- 반복 실행 result digest 안정성
- cache budget 초과 스트레스

## 16. 성능 가설

다음 가설은 실험 전 주장으로 확정하면 안 되며, 통계로 검증해야 한다.

1. 현재 `ours`는 이미 DFS 확장마다 exact temporal intersection을 수행하므로 단순 “early temporal
   pruning 추가”만으로는 새로운 향상이 거의 없다.
2. TD-tree block이 parent-binding 구조 후보를 이미 저장하므로 tree/path query에서는 구조 cache의
   추가 이득이 작다.
3. cycle/dense query에서는 여러 already-mapped neighbors의 adjacency intersection과 시간 지원
   계산이 반복되므로 DAG와 memo 이득이 커질 가능성이 높다.
4. 작은 `k`는 partial states가 많아 reuse 기회가 커지지만 pruning은 약하다. 큰 `k`는 pruning은
   강하지만 cache hit 기회가 감소할 수 있다.
5. 방향과 라벨 제약은 Gopher의 무방향·비라벨 주 실험보다 공유 기회를 줄인다. Gopher 논문의
   11.76x 수치를 ours에 그대로 기대하면 안 된다.
6. reuse-aware order는 단순 tree보다 non-tree edge가 많고 query가 복잡할 때 유리할 가능성이 높다.
7. 무제한 temporal cache는 interval 결과 크기 때문에 느려질 수 있으며 bounded admission이 필요하다.
8. CPU 병렬성은 root/frontier task가 충분할 때 개선되지만, skew와 shared-cache contention으로
   작은 query에서는 역효과가 날 수 있다.
9. GPU는 큰 batch, 고차수 adjacency, 많은 interval fragments에서만 유리할 가능성이 높다. 강한
   필터 뒤의 작은 request는 launch와 transfer overhead 때문에 CPU가 더 빠를 수 있다.

## 17. 실험 설계

### 17.1 `260408_cdh.pdf` 재현

- 데이터: btc, sof, w-t, su
- query size `|E_Q|` 변화
- `k` 변화
- query size와 `k` 동시 변화
- 필터 전/후 unique edge 수
- query order별 intermediate candidate 수
- end-to-end, preprocessing, TD-tree build, enumeration 분리
- original TD-Tree, ours, ours_gopher 비교

`260408_cdh.pdf`는 preprocessing/indexing 포함 시간을 주장하므로 포함/제외 시간을 모두 내고,
비교 본문에서는 같은 범위를 사용한다. Gopher 논문처럼 compile time을 제외한 수치도 보조 표로만
제공한다.

### 17.2 ours_gopher ablation

```text
V0: audited ours
V1: ours_gopher, memo off, paper order
V2: V1 + structural memo
V3: V2 + temporal-support memo
V4: V3 + full DAG memo/hash-consing
V5: V4 + reuse-aware fixed order
V6: V5 + CPU parallel scheduler
VG: V5 + ROCm backend
```

`V1`은 DAG 실행 자체의 overhead를 보여준다. `V2`–`V4`는 후보 수가 아니라 physical operation
수를 줄이는 단계다. `V5`는 탐색 순서가 바뀌므로 DFS states와 candidate 수까지 줄 수 있다.

### 17.3 추가 독립 변수

- topology: path, star, tree, cycle, triangle, clique, house, winged/dense pattern
- 같은 edge count를 가진 sparse/dense query 쌍
- `k = 2,3,5,10` 및 데이터가 허용하는 더 큰 값
- cache budget `0,64,256,1024 MiB`
- CPU threads `1,2,4,8,physical cores`
- interval fragmentation
- degree skew
- label selectivity와 repeated-label query
- directed reciprocal-edge 비율

### 17.4 실행 규약

- 성능 run은 `--count-only`를 모든 방법에 동일하게 적용한다.
- 결과 파일 I/O 시간은 별도 열로 두며 알고리즘 시간과 섞지 않는다.
- cold cache 기준으로 각 query 실행 전 memo를 비운다.
- 최소 1회 warm-up 후 3–5회 측정하고 median, min/max 또는 IQR을 기록한다.
- 같은 label seed, query file, `k`, compiler optimization, thread affinity를 사용한다.
- timeout/OOM은 값 0이 아니라 명시 상태로 기록한다.
- 모든 run은 result count와 digest가 기준선과 같은지 먼저 확인한다.

Excel에는 최소한 다음 sheet가 필요하다.

```text
Environment
Dataset_Stats
Correctness
Paper_Reproduction
Ablation
Query_Topology
K_Sweep
Cache_Budget
CPU_Scaling
GPU_CPU
Operation_ReUse
Memory
Raw_Runs
```

## 18. ROCm 설계

### 18.1 현재 환경 확인

2026-07-28 현재 실행 컨텍스트에서는 다음 상태가 확인됐다.

- `/opt/rocm/bin/hipcc`, `rocminfo`, `rocm-smi`, `amd-smi`가 설치되어 있다.
- ROCm 버전은 도구 출력 기준 7.2.4다.
- `/dev/kfd`와 `/dev/dxg`가 보이지 않는다.
- `rocminfo`는 `ROCk module is NOT loaded, possibly no GPU devices`를 출력한다.
- `rocm-smi`는 amdgpu driver가 초기화되지 않았다고 출력한다.

즉, toolchain은 있지만 현재 컨테이너/WSL 실행 컨텍스트에서는 GPU runtime을 사용할 수 없다.
GPU 폴더 구현은 가능하지만 실제 GPU 성능 수치는 장치가 노출된 환경에서만 얻을 수 있다.

### 18.2 `Ours_DFS` CUDA 코드를 그대로 쓰지 않는 이유

참고 구현은 각 호출마다 vertex arrays와 CSR 전체를 device로 복사하고, root/child의 약한
label/degree/duration prefilter만 kernel로 실행한다. 또한 CUDA 전용이며 ROCm 대상이 아니다.
이 방식은 작은 candidate expansion마다 전송 비용이 생겨 Gopher식 공통 계산 재사용과 맞지 않는다.

### 18.3 device-resident graph

다음 배열은 query 실행 전에 한 번만 GPU에 올린다.

```text
out_offsets, out_neighbors, out_edge_ids
in_offsets,  in_neighbors,  in_edge_ids
vertex_labels
out/in degrees and directed NLF statistics
edge_interval_offsets
interval_starts, interval_ends
```

query plan, DAG nodes, dependency descriptors도 작은 constant/read-only buffer로 올린다.

### 18.4 hybrid execution

전체 DFS를 GPU에 옮기지 않는다. irregular recursion, 작은 query, 가변 결과 크기 때문에 CPU
scheduler가 search state를 유지하고 GPU에는 비싼 DAG memo misses를 batch로 보낸다.

```text
CPU workers
  -> collect structural/temporal DAG requests
  -> deduplicate identical RuntimeKeys within batch
  -> submit unique requests

GPU
  -> batched sorted adjacency intersections
  -> batched interval intersections + longest run
  -> count/scan/write variable-length outputs

CPU
  -> put immutable results into shared memo
  -> scatter one result to all waiting DFS states
```

두 개 이상의 HIP stream과 pinned double buffers로 request 생성, kernel, 결과 회수를 겹친다.
작은 batch는 CPU로 fallback한다.

### 18.5 GPU kernel 후보

1. sorted candidate-adjacency intersection
2. 여러 adjacency list의 staged intersection
3. edge interval list intersection
4. longest consecutive interval reduction
5. candidate flag compaction을 위한 count + prefix scan + write
6. 고차수 request partition과 결과 merge

interval 결과는 boolean만 반환하면 안 된다. 이후 query edge와 다시 교차해야 하므로 정확한
interval list를 보존해야 한다.

가변 길이 출력은 두 단계로 처리한다.

```text
pass 1: 각 request의 output count
exclusive scan
pass 2: compact output write
```

batch 안의 key dedup은 CPU unordered map으로 먼저 구현한다. 이후 고정 길이 binding key가
충분히 많을 때 rocPRIM sort/unique를 실험할 수 있다. hash만으로 GPU 결과를 합치지 않고 full
binding tuple equality를 확인한다.

### 18.6 offload 기준

초기 auto 정책은 측정 가능한 work 기준이어야 한다.

```text
offload if
  request_count >= gpu_batch_threshold
  OR total_input_elements >= gpu_min_work
otherwise CPU
```

권장 시작값은 `request_count=4096`, `total_input_elements=1,000,000`이며, 이는 고정 결론이 아니라
microbenchmark로 조정한다.

### 18.7 GPU parity

- 모든 kernel에 CPU reference function을 둔다.
- random candidate/adjacency/interval batch를 CPU와 bit-exact 비교한다.
- `--device gpu`와 `--device cpu`의 final result set, count, digest를 비교한다.
- device가 없으면 명시적으로 `SKIPPED_NO_DEVICE`를 기록하고 CPU fallback을 GPU 성능으로
  보고하지 않는다.

## 19. 구현 순서

1. 감사 완료된 `ours`를 기준으로 parity harness와 상세 통계를 추가한다.
2. `B(u,φ)`를 legacy 계산과 나란히 구현하고 shadow verification한다.
3. bounded structural memo를 추가한다.
4. `S(u->v|φ)` 계산을 분리하고 bounded temporal memo를 추가한다.
5. 두 캐시의 static signature/dependency binding 코드를 공통화한다.
6. `CandidateDag`와 canonical fingerprint/hash-consing으로 일반화한다.
7. `paper` fixed order에서 전체 oracle/parity test를 통과시킨다.
8. reuse-aware **fixed** plan을 추가하고 별도 ablation한다.
9. root/frontier CPU task scheduler와 shared memo를 추가한다.
10. 실제 중복률과 weighted saved work가 충분한지 확인한 뒤 HIP backend를 구현한다.
11. 모든 성능 실험 전에 correctness sheet를 완성한다.

이 순서를 지키면 pruning으로 논리 탐색량이 줄어든 효과, memo로 실제 연산량이 줄어든 효과,
schedule과 병렬화로 wall-clock이 줄어든 효과를 서로 구분할 수 있다.
