#include <improbable/worker.h>
#include <improbable/standard_library.h>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <algorithm>
#include <ctime>

#include <chrono>
#include <mutex>
#include <queue>
#include <sstream>
#include <thread>
#include <unordered_set>

// Use this to make a worker::ComponentRegistry.
// For example use worker::Components<improbable::Position, improbable::Metadata> to track these common components
using ComponentRegistry = worker::Components<improbable::Position, improbable::Metadata>;

// Constants and parameters
const int ErrorExitStatus = 1;
const std::string kLoggerName = "startup.cc";
const std::uint32_t kGetOpListTimeoutInMilliseconds = 100;

worker::Connection ConnectWithReceptionist(const std::string hostname,
                                           const std::uint16_t port,
                                           const std::string& worker_id,
                                           const worker::ConnectionParameters& connection_parameters) {
    auto future = worker::Connection::ConnectAsync(ComponentRegistry{}, hostname, port, worker_id, connection_parameters);
    return future.Get();
}

std::string get_random_characters(size_t count) {
    const auto randchar = []() -> char {
        const char charset[] =
            "0123456789"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz";
        const auto max_index = sizeof(charset) - 1;
        return charset[rand() % max_index];
    };
    std::string str(count, 0);
    std::generate_n(str.begin(), count, randchar);
    return str;
}

template <typename Rep, typename Period>
std::string duration_str(const std::chrono::duration<Rep, Period> &d) {
    std::stringstream s;
    s << d.count() << "(" << Period::num << "/" << Period::den << ")s";
    return s.str();
}

// Entry point
int main(int argc, char** argv) {
    srand(time(nullptr));

    std::cerr << "[local] Worker started " << std::endl;

    auto print_usage = [&]() {
        std::cout << "Usage: " << argv[0] << " receptionist <hostname> <port> <worker_id>" << std::endl;
        std::cout << std::endl;
        std::cout << "Connects to SpatialOS" << std::endl;
        std::cout << "    <hostname>      - hostname of the receptionist or locator to connect to.";
        std::cout << std::endl;
        std::cout << "    <port>          - port to use if connecting through the receptionist.";
        std::cout << std::endl;
        std::cout << "    <worker_id>     - (optional) name of the worker assigned by SpatialOS." << std::endl;
        std::cout << std::endl;
    };

    std::vector<std::string> arguments;

    // if no arguments are supplied, use the defaults for a local deployment
    if (argc == 1) {
        arguments = { "receptionist", "localhost", "7777" };
    } else {
        arguments = std::vector<std::string>(argv + 1, argv + argc);
    }

    if (arguments.size() != 4 && arguments.size() != 3) {
        print_usage();
        return ErrorExitStatus;
    }

    worker::ConnectionParameters parameters;
    parameters.WorkerType = "SpawnerReaper";
    parameters.Network.ConnectionType = worker::NetworkConnectionType::kTcp;
    parameters.Network.UseExternalIp = false;

    std::string workerId;

    // When running as an external worker using 'spatial local worker launch'
    // The WorkerId isn't passed, so we generate a random one
    if (arguments.size() == 4) {
        workerId = arguments[3];
    } else {
        workerId = parameters.WorkerType + "_" + get_random_characters(4);
    }

    std::cerr << "[local] Connecting to SpatialOS as " << workerId << "..." << std::endl;

    // Connect with receptionist
    worker::Connection connection = ConnectWithReceptionist(arguments[1], atoi(arguments[2].c_str()), workerId, parameters);

    connection.SendLogMessage(worker::LogLevel::kInfo, kLoggerName, "Connected successfully");

    // Register callbacks and run the worker main loop.
    worker::Dispatcher dispatcher{ ComponentRegistry{} };
    bool is_connected = connection.IsConnected();

    dispatcher.OnDisconnect([&is_connected](const worker::DisconnectOp& op) {
        std::cerr << "[disconnect] " << op.Reason << std::endl;
        is_connected = false;
    });

    // Print log messages received from SpatialOS
    dispatcher.OnLogMessage([](const worker::LogMessageOp& op) {
        if (op.Level == worker::LogLevel::kFatal) {
            std::cerr << "Fatal error: " << op.Message << std::endl;
            std::terminate();
        }
        std::cerr << "[remote] " << op.Message << std::endl;
    });

    // Track creation requests and created entities.
    std::unordered_set<worker::RequestId<worker::CreateEntityRequest>> create_requests;
    std::mutex create_requests_mutex;  // guards create_requests

    // Add an event handler for OnCreateEntityResponse.
    //
    // We put created entities in a priority queue, with earlier entities
    // earlier.  We will later use this to reap (delete) entities that we
    // created.
    //
    // This is a quick & dirty hack, not a good worker design, because an
    // entity's age lives only in the memory space of the worker process that
    // created it.  Workers may be freely killed and restarted at any time by
    // SpatialOS, so if the worker that created an entity were to die, then the
    // creation age would be forgotten, and no replacement worker would ever be
    // able to reap it.
    //
    // A better design would treat the worker's memory as ephemeral.  Since we
    // want an entity's creation time to determine its fate even across worker
    // restarts, this state should be saved in a component attached to the
    // entity.  Then, each worker should inspect the entities in its authority,
    // and reap old entities.  We might implement this in a future revision of
    // this demo.
    using timestamp = std::chrono::time_point<std::chrono::steady_clock>;
    using timestamped_entity = std::pair<worker::EntityId, timestamp>;
    auto priority_greater = [](const timestamped_entity &a, const timestamped_entity &b) {
        return a.first > b.first;
    };
    std::priority_queue<
        timestamped_entity,
        std::vector<timestamped_entity>,
        decltype(priority_greater)
    > created(priority_greater);
    std::mutex created_mutex;
    dispatcher.OnCreateEntityResponse(
        [
            &create_requests,
            &create_requests_mutex,
            &created,
            &created_mutex
        ]
        (const worker::CreateEntityResponseOp& op) {
            auto ts = std::chrono::steady_clock::now();

            bool ignore = false;
            {
                std::lock_guard<std::mutex> lock(create_requests_mutex);
                auto req = create_requests.find(op.RequestId);
                ignore = req == create_requests.end();
                if (!ignore) {
                    create_requests.erase(req);
                }
            }

            // We pull this stanza out of the critical section to avoid holding the lock longer.
            if (ignore) {
                std::cerr << "[local] ignoring create request that we did not trigger: " << op.RequestId.Id << std::endl;
                return;
            }
            if (op.StatusCode != worker::StatusCode::kSuccess) {
                std::cerr << "[local] failed creation request: " << op.RequestId.Id
                          << "; reason: " << op.Message
                          << std::endl;
                return;
            }

            {
                std::lock_guard<std::mutex> lock(create_requests_mutex);
                created.emplace(*op.EntityId, ts);
            }
        }
    );

    // Connections are not thread-safe.  In the code above, we have relied on
    // the fact that, during startup, the main thread is the only thread.
    // However, we are about to define the spawning/reaping loop, which will run
    // on its own thread, so we need a mutex now.
    //
    // This mutex guards access to the connection object after multiple threads
    // are spun up.
    //
    // If you were writing a lot more code, you'd probably write a utility class
    // that performs mutex acquisition/release while acting as a Connection
    // handle, and make sure that all Connection access goes through this class.
    std::mutex connection_mutex;

    auto tick_fn = [
        &connection,
        &connection_mutex,
        &create_requests,
        &create_requests_mutex,
        &created,
        &created_mutex
    ]() {
        while (true) {
            // This is a very dumb simulation loop that just does some stuff and
            // then sleeps for a second.  A proper loop would calibrate the
            // "frame rate"/sleep time based on the actual time elapsed in the
            // body of the loop below.
            std::this_thread::sleep_for(std::chrono::seconds(1));

            {
                std::lock_guard<std::mutex> lock(connection_mutex);
                // The docs are wrong: worker::LogLevel enum cases use
                // Google-style "kFooBar" naming, not "FooBar".
                connection.SendLogMessage(worker::LogLevel::kInfo, "timer", "tick (will try to create an entity)");
            }

            // Set up a dummy entity, positioned randomly in a 5x5 square.
            worker::Entity ent;
            improbable::Position::Data pos;
            // Note that Y is "up" in SpatialOS, so X & Z translate
            // horizontally, i.e. the spatial inspector's coordinate system
            // looks like this:
            //
            //      Z
            //      ^
            //      |
            //   <--+--> X
            //      |
            //      v
            //
            // with Y projecting "out of the screen".
            pos.set_coords(improbable::Coordinates{double(rand() % 5), 0, double(rand() % 5)});
            ent.Add<improbable::Position>(pos);
            improbable::Metadata::Data meta;
            meta.set_entity_type("ticktock");
            ent.Add<improbable::Metadata>(meta);

            // Send the creation request.
            worker::RequestId<worker::CreateEntityRequest> create_req_id;
            {
                std::lock_guard<std::mutex> lock(connection_mutex);
                create_req_id = connection.SendCreateEntityRequest(ent, {}, {});
            }
            {
                std::lock_guard<std::mutex> lock(create_requests_mutex);
                create_requests.insert(create_req_id);
            }
            std::cerr << "[local] sent entity creation request: " << create_req_id.Id << std::endl;

            // Reap any entities <10s old
            std::vector<worker::EntityId> to_reap;
            const auto now = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(created_mutex);
                while (!created.empty()) {
                    const auto top = created.top();
                    if (now - top.second < std::chrono::seconds(10)) {
                        break;
                    } else {
                        created.pop();
                        to_reap.push_back(top.first);
                    }
                }
            }
            while (!to_reap.empty()) {
                auto back = to_reap.back();
                to_reap.pop_back();
                std::cerr << "[local] reaping: " << back << std::endl;
                // TODO(keunwoo): Add logic that buffers outstanding deletion requests
                // and retries them on failure.
                {
                    std::lock_guard<std::mutex> lock(connection_mutex);
                    connection.SendDeleteEntityRequest(back, {});
                }
            }
        }
    };

    if (is_connected) {
        std::cerr << "[local] starting timer thread..." << std::endl;
        std::thread timer(tick_fn);
        timer.detach();

        std::cerr << "[local] Connected successfully to SpatialOS, listening to ops... " << std::endl;
    }

    while (is_connected) {
        worker::Option<worker::OpList> op_list;
        {
            std::lock_guard<std::mutex> lock(connection_mutex);
            op_list.emplace(connection.GetOpList(kGetOpListTimeoutInMilliseconds));
        }
        dispatcher.Process(*op_list);
    }
    exit(ErrorExitStatus);
}
