import os
import re
import shutil
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CORE_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, ".."))

# 설정
SOURCE_DIR = os.path.join(CORE_ROOT, "external", "boost", "boost")  # 현재 헤더가 있는 위치
OUTPUT_DIR = os.path.join(CORE_ROOT, "external", "boost_subset", "boost") # 복사될 위치
ROOT_HEADERS = [
    'asio.hpp',
    'beast.hpp',
    'system/error_code.hpp', # 자주 누락되는 system 관련 헤더 명시
    'version.hpp',
    'config.hpp'
]

# 정규식: #include <boost/...> 또는 "boost/..."
INCLUDE_PATTERN = re.compile(r'^\s*#\s*include\s+["<](boost/[^">]+)[">]')

# 방문한 파일 집합 (중복 방지)
visited = set()
files_to_process = []

def get_full_path(rel_path):
    return os.path.join(CORE_ROOT, "external", "boost", rel_path)

def process_file(rel_path):
    if rel_path in visited:
        return
    visited.add(rel_path)
    
    src_path = get_full_path(rel_path)
    
    if not os.path.exists(src_path):
        # 헤더가 없는 경우 (platform specific 등) 경고만 출력하고 패스
        # print(f"Warning: File not found: {src_path}")
        return

    # 파일 읽기
    try:
        with open(src_path, 'r', encoding='utf-8', errors='ignore') as f:
            lines = f.readlines()
    except Exception as e:
        print(f"Error reading {src_path}: {e}")
        return

    # 파싱
    for line in lines:
        match = INCLUDE_PATTERN.match(line)
        if match:
            include_rel_path = match.group(1)
            if include_rel_path not in visited:
                files_to_process.append(include_rel_path)

    # 복사 (Output Dir 구조 생성)
    dest_path = os.path.join(CORE_ROOT, "external", "boost_subset", rel_path)
    os.makedirs(os.path.dirname(dest_path), exist_ok=True)
    shutil.copy2(src_path, dest_path)

def main():
    print(f"Scanning dependencies starting from: {ROOT_HEADERS}")
    
    # 초기 큐 채우기
    for h in ROOT_HEADERS:
        files_to_process.append(f"boost/{h}")

    # BFS 탐색
    count = 0
    while files_to_process:
        current_file = files_to_process.pop(0)
        process_file(current_file)
        count += 1
        if count % 100 == 0:
            print(f"Processed {count} files...", end='\r')

    print(f"\nCompleted. Total files extracted: {len(visited)}")
    
    # 원본 vs 최적화 용량 비교
    original_size = get_dir_size(os.path.join(CORE_ROOT, "external", "boost"))
    new_size = get_dir_size(os.path.join(CORE_ROOT, "external", "boost_subset"))
    
    print(f"Original Size: {original_size / 1024 / 1024:.2f} MB")
    print(f"Subset Size:   {new_size / 1024 / 1024:.2f} MB")
    print(f"Reduction:     {100 - (new_size/original_size*100):.1f}%")

def get_dir_size(start_path):
    total_size = 0
    for dirpath, dirnames, filenames in os.walk(start_path):
        for f in filenames:
            fp = os.path.join(dirpath, f)
            if not os.path.islink(fp):
                total_size += os.path.getsize(fp)
    return total_size

if __name__ == "__main__":
    boost_subset = os.path.join(CORE_ROOT, "external", "boost_subset")
    if os.path.exists(boost_subset):
        shutil.rmtree(boost_subset)
    main()
