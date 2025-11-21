import argparse
import asyncio
import logging
import os
import sys
import json
import yaml
from pathlib import Path
import torch

# Add E2GraphRAG to path
PROJECT_ROOT = Path(__file__).resolve().parent
E2_PATH = PROJECT_ROOT / "E2GraphRAG-main"
if str(E2_PATH) not in sys.path:
    sys.path.insert(0, str(E2_PATH))

# Add rag_backup/src to path for VLM parser reuse if needed
BACKUP_SRC_PATH = PROJECT_ROOT.parent / "rag_backup" / "src"
if str(BACKUP_SRC_PATH) not in sys.path:
    sys.path.insert(0, str(BACKUP_SRC_PATH))

from document_loader import DocumentProcessor
from api_client import APIClient
from vlm_parser import VLMParser

# Import E2GraphRAG query components
try:
    from query import Retriever
    from extract_graph import load_nlp
    from prompt_dict import Prompts
except ImportError:
    print("Warning: Failed to import E2GraphRAG components. Make sure E2GraphRAG-main is in the directory.")

logger = logging.getLogger(__name__)

def load_config(config_path):
    with open(config_path, "r") as f:
        return yaml.safe_load(f)

async def main_async(args):
    config = load_config(args.config)
    
    # 1. Initialize VLM Context
    api_client = APIClient(config)
    vlm_parser = VLMParser(vision_model_func=api_client.call_vlm)
    
    # 2. Process Documents
    processor = DocumentProcessor(config, vlm_parser)
    if not args.skip_ingest:
        await processor.process_dataset(
            config["dataset"]["dataset_path"], 
            rebuild=args.rebuild
        )
    
    # 3. Query / Evaluation
    if args.questions:
        with open(args.questions, 'r') as f:
            # rag_backup questions.json is a list of strings
            questions = json.load(f) 
        
        if not isinstance(questions, list):
            logger.error("Questions file must contain a list of strings or dicts")
            return

        # Load NLP once
        nlp = load_nlp()
        
        # Identify all processed docs
        doc_dirs = sorted([d for d in (Path(config["paths"]["cache_path"]) / config["dataset"]["dataset_name"]).iterdir() if d.is_dir()])
        
        # We need to load the Graph/Tree for each doc to build retrievers
        # Since we can't keep 100 graphs in memory easily if they are huge, maybe we load one by one?
        # But for 14 questions, iterating docs for each question is O(Q*D).
        # Loading graph takes time.
        # Better: Load all Retrievers into memory (if they fit).
        
        retrievers = []
        doc_names = []
        
        for doc_dir in doc_dirs:
            try:
                # Load cached data
                # E2GraphRAG caches: 'tree.pkl', 'graph.pkl' (G, index, appearance_count)
                import pickle
                
                tree_path = doc_dir / "tree.pkl"
                graph_path = doc_dir / "graph.pkl"
                
                if not tree_path.exists() or not graph_path.exists():
                    logger.warning(f"Skipping incomplete cache: {doc_dir}")
                    continue
                
                with open(tree_path, "rb") as f:
                    tree = pickle.load(f)
                
                with open(graph_path, "rb") as f:
                    G, index, appearance_count = pickle.load(f)
                
                # Initialize Retriever
                retriever = Retriever(
                    tree, G, index, appearance_count, nlp, 
                    **config["retriever"]["kwargs"]
                )
                retrievers.append(retriever)
                doc_names.append(doc_dir.name)
                logger.info(f"Loaded retriever for {doc_dir.name}")
                
            except Exception as e:
                logger.error(f"Failed to load {doc_dir.name}: {e}")

        answers = []
        api_client = APIClient(config) # Reuse for QA generation
        
        for idx, question_item in enumerate(questions, 1):
            # Handle both string and dict formats
            if isinstance(question_item, dict):
                question = question_item.get("question", "")
            else:
                question = str(question_item)
                
            logger.info(f"Querying {idx}/{len(questions)}: {question}")
            
            all_chunks = []
            
            # Retrieve from all docs
            for i, retriever in enumerate(retrievers):
                try:
                    result = retriever.query(question, **config["retriever"]["kwargs"])
                    # result['chunks'] is a string formatted with "key: content"
                    # We need to parse it back or just append it
                    # result also has 'chunk_ids' if debug=True
                    
                    # Let's store the raw chunks and the source doc
                    if "chunks" in result and result["chunks"].strip():
                        all_chunks.append(f"Source: {doc_names[i]}\n{result['chunks']}")
                except Exception as e:
                    logger.error(f"Query failed for {doc_names[i]}: {e}")

            # Aggregate context
            # If too long, truncate?
            # Simple concatenation for now
            combined_context = "\n\n".join(all_chunks)
            
            # Generate Answer
            # We need a prompt. Let's use a simple one or borrow from backup.
            prompt = f"""Based on the following context, answer the question.
If the answer is not in the context, say "I don't know".

Context:
{combined_context}

Question: {question}
Answer:"""

            messages = [{"role": "user", "content": prompt}]
            answer_text = await api_client.call_vlm(messages)
            
            # Format for output
            # backup expects: { "question": ..., "answer": ..., "filename": ..., "page": ... }
            # Our current logic doesn't easily track "best filename/page" yet without parsing chunks.
            # E2GraphRAG chunks text is just text. 
            # But we embedded metadata in document_loader: [SOURCE:x|PAGE:y]
            # If E2GraphRAG preserved that text, we can regex it from the answer context.
            
            # Basic extraction of filename/page from the *context* used? 
            # For now, leave filename/page empty or try to guess from the most relevant chunk.
            
            answers.append({
                "question": question,
                "answer": answer_text,
                "filename": "", # TODO: Extract from context
                "page": 0       # TODO: Extract from context
            })

        # Save output
        output_path = config.get("dataset", {}).get("output_path", args.output) # args.output doesn't exist in parser yet
        # Fix: args.output wasn't added to parser in previous edit
        output_path = "./data/output.json" # Fallback
        
        with open(output_path, "w") as f:
            json.dump(answers, f, indent=4, ensure_ascii=False)
        logger.info(f"Saved answers to {output_path}")

if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default="competition_config.yaml")
    parser.add_argument("--questions", default="./data/questions.json")
    parser.add_argument("--skip-ingest", action="store_true")
    parser.add_argument("--rebuild", action="store_true")
    args = parser.parse_args()
    
    asyncio.run(main_async(args))
