# Graph Path Finding Server and Client
## Project Definition
This project implements a multi-threaded server and client system for finding paths in a large graph. The server loads a graph from a file, handles multiple client connections concurrently, and uses caching to improve performance. The client requests paths between two nodes in the graph from the server.
## Key Features:
* Multi-threaded server with dynamic thread pool
* Efficient graph loading and path finding using Breadth-First Search (BFS)
* Caching of previously calculated paths
* Configurable reader/writer priority for cache access
* Daemonization of the server process
* Detailed logging of server and client activities
## Usage Instructions
### Server
To start the server, use the following command:
` ./server -i pathToFile -p PORT -o pathToLogFile -s initialThreads -x maxThreads [-r priorityMode] `

Parameters:
* -i: Path to the input graph file
* -p: Port number for the server to listen on
* -o: Path to the log file
* -s: Initial number of threads in the pool (minimum 2)
* -x: Maximum number of threads allowed
* -r: (Optional) Priority mode for reader/writer lock (0: reader priority, 1: writer priority, 2: equal priority)

` ./server -i /home/user/graph.dat -p 34567 -o /home/user/server.log -s 8 -x 24 -r 1 `

### Client
To run the client, use the following command:

`  ./client -a IP_ADDRESS -p PORT -s SOURCE_NODE -d DEST_NODE` 

Parameters:
* -a: IP address of the server
* -p: Port number of the server
* -s: Source node for the path
* -d: Destination node for the path

Example:

` ./client -a 127.0.0.1 -p 34567 -s 768 -d 979`

## Implementation Details
### Server
1. Initialization:
  * Parses command-line arguments
  * Creates a lockfile to prevent multiple instances
  * Daemonizes the process
  * Redirects output to the specified log file
  * Loads the graph from the input file
  * Initializes the thread pool and cache
2. Graph Loading:
  * Reads edges from the input file
  * Constructs an adjacency list representation of the graph
3. Thread Pool:
  * Initializes with a specified number of threads
  * Dynamically resizes up to a maximum limit based on server load
  * Uses a resizer thread to monitor and adjust pool size
4. Connection Handling:
  * Accepts client connections
  * Assigns connections to available threads in the pool
5. Path Finding:
  * Uses Breadth-First Search (BFS) to find the shortest path between two nodes
  * Caches calculated paths for future use
6. Caching:
  * Implements a hash table for quick path lookup
  * Uses a custom readers-writer lock for thread-safe access
  * Supports configurable priority modes for readers and writers
7. Logging:
  * Records detailed information about server activities
  * Includes timestamps for each log entry
### Client
1. Initialization:
  * Parses command-line arguments
  * Establishes a connection to the server
2. Request Handling:
  * Sends source and destination nodes to the server
  * Receives and interprets the server's response
3. Output:
  * Displays the path found or "NO PATH" message
  * Shows the time taken for the server to respond
4. Logging:
  * Logs each step of the client's operation with timestamps
## Performance Considerations
* The server uses a thread pool to handle multiple connections efficiently.
* Caching of paths reduces computation time for repeated queries.
* The dynamic thread pool allows the server to adapt to varying loads.
* The custom readers-writer lock implementation allows for fine-tuned control over cache access priorities.
## Error Handling
* Both server and client implement robust error checking and logging.
* The server gracefully handles termination signals, ensuring proper cleanup.
* The client reports connection errors and invalid server responses.
