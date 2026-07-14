# Ours_DFS

`ours`를 분리 복제한 뒤, 다음 두 가지를 반영한 포트입니다.

- 질의 분해, TD-tree 성장 순서, 최종 매칭 열거를 BFS 중심으로 재구성
- 후보 필터링을 GPU에서 돌릴 수 있도록 CUDA 레이어 추가

## 구성

- `query_decomposition.cpp`
  - 기존 DFS 스패닝트리 생성을 BFS로 변경
  - `root`, `parent`, `level`, `traversal_order`를 함께 저장
- `TDTree.cpp`
  - 성장 순서와 트리밍을 BFS/역-BFS 기반으로 정리
  - 최종 매칭 열거를 재귀 DFS 대신 큐 기반 BFS frontier 확장으로 변경
- `GpuCandidateExpander.*`
  - 그래프를 정수 배열 기반으로 전처리
  - CUDA 빌드 시 루트/자식 후보 프리필터를 GPU에서 수행
  - CUDA를 못 쓰는 환경에서는 같은 인터페이스로 CPU fallback

## 빌드

CPU fallback 빌드:

```powershell
.\build_cpu.ps1
```

CUDA 빌드:

```powershell
.\build_cuda.ps1
```

주의:

- 현재 저장소 환경에서는 `nvcc`는 보이지만 `cl.exe`가 PATH에 없어 CUDA 빌드는 Visual Studio C++ Build Tools가 준비된 셸에서 실행해야 합니다.
- 그래서 기본 검증은 `build_cpu.ps1` 기준으로 맞춰 두었습니다.

## 실행

```powershell
.\td_tree_bfs.exe ..\Dataset\testdata.txt ..\Dataset\Query3.txt 3
```

CUDA 빌드가 성공했다면:

```powershell
.\td_tree_bfs_cuda.exe ..\Dataset\testdata.txt ..\Dataset\Query3.txt 3
```

## 배치 실행

```powershell
.\run_unique_filtered.ps1
```

기본 실행 파일 경로는 `.\td_tree_bfs.exe`입니다. CUDA 실행 파일을 쓰려면 `-ExecutablePath`를 넘기면 됩니다.
