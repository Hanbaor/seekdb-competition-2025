import json
from typing import List


class Answer(object):
    question: str = ""
    answer: str = ""
    filename: str = ""
    page: int = 0
    retrieval_results: List[dict] = None  # Store detailed retrieval results
    hypothetical_answer: str = ""  # Store hypothetical answer for HyDE retrieval

    def __init__(
        self, question: str = "", answer: str = "", filename: str = "", page: int = 0, retrieval_results: List[dict] = None, hypothetical_answer: str = ""
    ):
        self.question = question
        self.answer = answer
        self.filename = filename
        self.page = page
        self.retrieval_results = retrieval_results or []
        self.hypothetical_answer = hypothetical_answer


def read_train_json(filepath: str) -> List[Answer]:
    """
    Read training data from JSON file.

    Args:
        filepath: Path to the training JSON file

    Returns:
        List of Answer objects

    Raises:
        TypeError: If the JSON format is invalid
        FileNotFoundError: If the file doesn't exist
    """
    with open(filepath, "r", encoding="utf-8") as f:
        data = f.read()

    json_data = json.loads(data)
    if isinstance(json_data, list):
        return [Answer(**item) for item in json_data]
    elif isinstance(json_data, dict):
        return [Answer(**json_data)]
    else:
        raise TypeError(
            f"Error decoding train.json: expected list or dict, got {type(json_data)}"
        )


def write_answers(answers: List[Answer], filename: str, include_retrieval: bool = False) -> None:
    """
    Write answers to JSON file.

    Args:
        answers: List of Answer objects to write
        filename: Path to the output JSON file
        include_retrieval: Whether to include retrieval results in output

    Raises:
        IOError: If the file cannot be written
    """
    # Convert Answer objects to dictionaries
    data = []
    for answer in answers:
        item = {
            "question": answer.question,
            "answer": answer.answer,
            "filename": answer.filename,
            "page": answer.page,
        }
        if include_retrieval:
            if answer.retrieval_results:
                item["retrieval_results"] = answer.retrieval_results
            if answer.hypothetical_answer:
                item["hypothetical_answer"] = answer.hypothetical_answer
        data.append(item)

    # Write to JSON file
    with open(filename, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)


def read_questions_json(filepath: str) -> List[str]:
    """
    Read questions from JSON file.

    Args:
        filepath: Path to the questions JSON file

    Returns:
        List of question strings

    Raises:
        TypeError: If the JSON format is invalid
        FileNotFoundError: If the file doesn't exist
    """
    with open(filepath, "r", encoding="utf-8") as f:
        data = f.read()

    json_data = json.loads(data)

    if isinstance(json_data, list):
        # Handle list of strings
        if len(json_data) > 0 and isinstance(json_data[0], str):
            return json_data
        # Handle list of objects with 'question' field
        elif len(json_data) > 0 and isinstance(json_data[0], dict):
            return [
                item.get("question", "") for item in json_data if "question" in item
            ]
        else:
            raise TypeError(
                f"Error decoding questions.json: expected list of strings or dicts with 'question' field"
            )
    elif isinstance(json_data, dict):
        # Handle single object with 'question' field
        if "question" in json_data:
            return [json_data["question"]]
        else:
            raise TypeError(
                f"Error decoding questions.json: dict must have 'question' field"
            )
    else:
        raise TypeError(
            f"Error decoding questions.json: expected list or dict, got {type(json_data)}"
        )
