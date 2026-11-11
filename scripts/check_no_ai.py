#!/usr/bin/env python3
"""Проверка требования ОБ-4: в CosmoCad нет искусственного интеллекта.

Сканирует файлы сборки и исходный код и завершается с ошибкой, если находит
подключение библиотек машинного обучения, обращения к ИИ-сервисам либо каталог
или модуль под ИИ. Запускается при каждой сборке (тест no_ai_dependencies).

Использование: check_no_ai.py <корень репозитория>
"""
import pathlib
import re
import sys

# Библиотеки и сервисы, подключение которых запрещено.
BANNED_NAMES = [
    "tensorflow", "torch", "pytorch", "libtorch", "onnx", "onnxruntime", "openvino", "tensorrt",
    "keras", "sklearn", "scikit-learn", "xgboost", "lightgbm", "catboost", "mxnet", "caffe", "jax",
    "transformers", "huggingface", "llama", "ggml", "gguf", "whisper", "mlpack", "dlib",
    "openai", "anthropic", "langchain", "ollama", "cohere", "mistralai", "gigachat", "yandexgpt",
    "api.openai.com", "generativelanguage.googleapis.com",
]

# Где ищем подключения: файлы сборки и строки подключения в исходниках.
BUILD_FILES = {"CMakeLists.txt", "conanfile.py", "conanfile.txt", "requirements.txt", "pyproject.toml",
               "vcpkg.json", "package.json"}
SOURCE_SUFFIXES = {".cpp", ".h", ".hpp", ".cc", ".py", ".cmake"}
INCLUDE_LINE = re.compile(r"^\s*(#\s*include|import|from|find_package|requires|FetchContent_Declare)\b", re.I)

# Каталоги и модули под ИИ создавать нельзя.
BANNED_DIRECTORIES = {"ai", "ml", "llm", "neural", "copilot", "assistant"}

SKIP_DIRECTORIES = {".git", "build", "third_party", "__pycache__"}
SELF = pathlib.Path(__file__).resolve()


def word_pattern(name: str) -> re.Pattern:
    return re.compile(r"(?<![A-Za-z0-9_])" + re.escape(name) + r"(?![A-Za-z0-9_])", re.I)


PATTERNS = [(name, word_pattern(name)) for name in BANNED_NAMES]


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    root = pathlib.Path(sys.argv[1]).resolve()
    problems = []

    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root)
        if any(part in SKIP_DIRECTORIES for part in relative.parts):
            continue
        if path.is_dir():
            if path.name.lower() in BANNED_DIRECTORIES:
                problems.append(f"{relative}: каталог под ИИ запрещён")
            continue
        if path.resolve() == SELF:
            continue
        is_build_file = path.name in BUILD_FILES
        if not is_build_file and path.suffix not in SOURCE_SUFFIXES:
            continue
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError as error:
            problems.append(f"{relative}: не удалось прочитать ({error})")
            continue
        for number, line in enumerate(lines, start=1):
            # В файлах сборки проверяем каждую строку, в исходниках — только строки подключения.
            if not is_build_file and not INCLUDE_LINE.match(line):
                continue
            for name, pattern in PATTERNS:
                if pattern.search(line):
                    problems.append(f"{relative}:{number}: запрещённая зависимость «{name}»: {line.strip()}")

    if problems:
        print("Нарушено требование ОБ-4 (никакого ИИ):")
        for problem in problems:
            print("  " + problem)
        return 1
    print("ОБ-4: зависимостей от ИИ не найдено.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
