#include <srtmux/transport.hpp>
#include <array>
#include <atomic>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>
using namespace srtmux;
using namespace std::chrono_literals;
void require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
template<class T> T take(Result<T> result) {
    if (auto* error = std::get_if<Error>(&result)) throw std::runtime_error(error->operation+": "+error->message);
    return std::get<T>(std::move(result));
}
void ready(Poller& p, Registration token, Interest interest) {
    std::array<Event, 4> events{}; Error detail{};
    auto result = p.wait(events, 3s, &detail);
    if (std::holds_alternative<IoError>(result)) throw std::runtime_error(detail.message);
    require(std::holds_alternative<size_t>(result), "readiness timed out");
    bool found = false;
    for (size_t i=0; i<std::get<size_t>(result); ++i)
        if (events[i].registration.id == token.id && contains(events[i].readiness, interest)) found = true;
    require(found, "wrong readiness/token");
}
void cancellation(Provider& provider) {
    auto p = take(provider.create_poller()); std::array<Event, 1> events{};
    require(std::holds_alternative<TimedOut>(p.wait(events, 0ms)), "empty poll timeout");
    take(p.wake()); take(p.wake());
    require(std::holds_alternative<Interrupted>(p.wait(events, InfiniteWait{})), "sticky wake");
    require(std::holds_alternative<TimedOut>(p.wait(events, 1ms)), "wake must coalesce");
    for (int i=0; i<20; ++i) {
        std::promise<void> entered;
        auto wait = std::async(std::launch::async, [&] { entered.set_value(); return p.wait(events, InfiniteWait{}); });
        entered.get_future().wait();
        take(p.wake());
        require(wait.wait_for(2s) == std::future_status::ready, "wake did not interrupt infinite wait");
        require(std::holds_alternative<Interrupted>(wait.get()), "incorrect interruption");
    }
    // A registration mutation wakes the native wait but is not user cancellation.
    auto listener = take(provider.listen({{"127.0.0.1", 0, 0}, 8, live_profile(), {}}));
    auto wait = std::async(std::launch::async, [&] { return p.wait(events, InfiniteWait{}); });
    auto token = take(p.add(listener, Interest::read));
    take(p.update(token, Interest::read | Interest::state));
    take(p.remove(token)); take(p.wake());
    require(std::holds_alternative<Interrupted>(wait.get()), "registration/wake race");
}
Listener profile_listener(Provider& p, const TransportConfig& cfg) {
    auto profile = take(p.create_profile(ProfileConfig::basic(cfg)));
    return take(p.listen_profile(profile, {"127.0.0.1",0,0}));
}
Connection profile_connection(Provider& p, const TransportConfig& cfg, const Endpoint& remote) {
    auto profile = take(p.create_profile(ProfileConfig::basic(cfg)));
    return take(p.connect_profile(profile, remote, Endpoint{"127.0.0.1",0,0}));
}
struct Pair {
    Listener listener;
    Connection tx;
    std::optional<Connection> rx;
    Poller sp, cp;
    Pair(Provider& caller, Provider& server, const TransportConfig& cfg, bool profiles = false)
        : listener(profiles ? profile_listener(server, cfg) : take(server.listen({{"127.0.0.1",0,0},8,cfg,{}}))),
          tx(profiles ? profile_connection(caller, cfg, take(listener.local_endpoint())) : take(caller.create_connection(cfg))),
          sp(take(server.create_poller())), cp(take(caller.create_poller())) {
        auto ltoken = take(sp.add(listener, Interest::read));
        auto ctoken = take(cp.add(tx, Interest::write | Interest::state));
        if (!profiles) {
            take(tx.bind({"127.0.0.1",0,0}));
            require(std::holds_alternative<Pending>(tx.connect(take(listener.local_endpoint()))), "connect pending");
        }
        ready(sp, ltoken, Interest::read);
        auto accepted = listener.accept();
        if (auto* e=std::get_if<Error>(&accepted)) throw std::runtime_error(e->message);
        require(std::holds_alternative<Connection>(accepted), "accept readiness");
        rx.emplace(std::get<Connection>(std::move(accepted)));
        ready(cp, ctoken, Interest::state);
        require(take(tx.info()).state == State::connected && take(rx->info()).state == State::connected, "connected state");
        take(sp.remove(ltoken)); take(cp.remove(ctoken));
    }
};
void transfer(Provider& caller, Provider& server, bool file, bool encrypted, bool profiles = false) {
    auto cfg = file ? file_stream_profile() : live_profile(); cfg.accounting = Accounting::payload_bytes;
    if (encrypted) {
        const std::string secret = "integration-test-secret";
        cfg.encryption = Encryption::aes_ctr_required; cfg.key_bits = 128;
        cfg.passphrase = take(Secret::copy(std::as_bytes(std::span(secret))));
    }
    require(take(caller.validate(Role::caller, cfg)).issues.empty(), "valid config");
    require(take(caller.capability(Feature::polling, Role::caller, cfg)).availability == Availability::available, "polling capability");
    Pair pair(caller, server, cfg, profiles);
    auto reg = take(pair.sp.add(*pair.rx, Interest::read));
    require(std::holds_alternative<Error>(pair.cp.add(*pair.rx, Interest::read)), "cross provider polling accepted");
    auto now = take(caller.now());
    std::array<std::byte, 1316> input{}, output{};
    for (size_t i=0;i<input.size();++i) input[i]=std::byte(i%251);
    if (file) {
        auto sent=pair.tx.write_stream(input); require(std::holds_alternative<size_t>(sent) && std::get<size_t>(sent)==input.size(), "write stream");
        size_t offset=0;
        while (offset<output.size()) {
            ready(pair.sp,reg,Interest::read);
            auto got=pair.rx->read_stream(std::span(output).subspan(offset));
            require(std::holds_alternative<size_t>(got), "read stream"); offset+=std::get<size_t>(got);
        }
    } else {
        auto wrong=now; ++wrong.clock_domain;
        require(std::holds_alternative<IoError>(pair.tx.send_message(input,{wrong})), "foreign clock accepted");
        require(std::holds_alternative<Success>(pair.tx.send_message(input,{now})), "send message");
        ready(pair.sp,reg,Interest::read);
        auto got=pair.rx->receive_message(output); require(std::holds_alternative<ReceivedMessage>(got), "receive message");
        require(std::get<ReceivedMessage>(got).bytes==input.size(), "message length");
    }
    require(input==output, "payload mismatch");
    require(take(pair.tx.statistics()).accepted_tx_bytes==input.size(), "tx accounting");
    require(take(pair.rx->statistics()).delivered_rx_bytes==input.size(), "rx accounting");
    auto metric = take(pair.tx.resolve_metric("native.byteSentUniqueTotal"));
    std::array<uint32_t, 1> ids{metric.id};
    std::array<srtmx_metric_v1, 1> metrics{}; metrics[0].struct_size = sizeof(metrics[0]);
    require(std::holds_alternative<TelemetryCapacity>(pair.tx.native_snapshot(ids, {})), "telemetry short output");
    auto snapshot = pair.tx.native_snapshot(ids, metrics);
    require(std::holds_alternative<srtmx_snapshot_v1>(snapshot) && metrics[0].value.u64 >= input.size(), "facade native counters");
    require(std::get<srtmx_snapshot_v1>(snapshot).clock_domain == now.clock_domain, "telemetry clock domain");
    // Race receive and send against abort-close on the same connection.
    std::atomic<bool> running{true}; std::atomic<unsigned> calls{0};
    std::jthread reader([&] { while (running.load()) { if(file) pair.rx->read_stream(output); else pair.rx->receive_message(output); ++calls; } });
    std::jthread writer([&] { while (running.load()) { if(file) pair.rx->write_stream(input); else pair.rx->send_message(input,{}); ++calls; } });
    while(calls.load()<100) std::this_thread::yield();
    require(std::holds_alternative<Success>(pair.rx->close(AbortClose{})), "concurrent abort close");
    running=false; reader.join(); writer.join();
    ready(pair.sp,reg,Interest::error);
    require(take(pair.rx->info()).state==State::closed, "closed info");
    require(std::holds_alternative<Success>(pair.rx->close_status()), "close status");
    take(pair.sp.remove(reg));
    std::array<Event,1> events{};
    require(std::holds_alternative<TimedOut>(pair.sp.wait(events,0ms)), "stale close event after remove");
}
void polling_batches(Provider& caller, Provider& server) {
    auto poller=take(server.create_poller());
    std::array<std::unique_ptr<Pair>,3> pairs;
    std::array<Registration,3> registrations{};
    std::array<bool,3> done{};
    for(size_t i=0;i<pairs.size();++i) {
        pairs[i]=std::make_unique<Pair>(caller,server,file_stream_profile());
        registrations[i]=take(poller.add(*pairs[i]->rx,Interest::read));
        std::array<std::byte,1> byte{std::byte(i+1)};
        auto sent=pairs[i]->tx.write_stream(byte);
        require(std::holds_alternative<size_t>(sent) && std::get<size_t>(sent)==1,"batch send");
    }
    size_t received=0;
    while(received<pairs.size()) {
        std::array<Event,3> events{}; // Non-power-of-two output capacity.
        auto result=poller.wait(events,3s);
        require(std::holds_alternative<size_t>(result),"batch readiness");
        for(size_t j=0;j<std::get<size_t>(result);++j) {
            size_t i=0; while(i<pairs.size() && registrations[i].id!=events[j].registration.id) ++i;
            require(i<pairs.size() && !done[i] && contains(events[j].readiness,Interest::read),"batch token/readiness");
            std::array<std::byte,1> byte{}; auto got=pairs[i]->rx->read_stream(byte);
            require(std::holds_alternative<size_t>(got) && std::get<size_t>(got)==1 && byte[0]==std::byte(i+1),"batch payload");
            take(poller.remove(registrations[i])); done[i]=true; ++received;
        }
    }
    // Remote failure must still report ERROR for a read-only subscription.
    auto token=take(poller.add(*pairs[0]->rx,Interest::read));
    require(std::holds_alternative<Success>(pairs[0]->tx.close(AbortClose{})),"peer close");
    ready(poller,token,Interest::error);
}
void terminal_fairness(Provider& provider) {
    Pair pair(provider, provider, file_stream_profile());
    auto poller = take(provider.create_poller());
    std::array<Connection, 3> closed{
        take(provider.create_connection(live_profile())),
        take(provider.create_connection(live_profile())),
        take(provider.create_connection(live_profile()))};
    std::array<Registration, 3> terminal{};
    for (size_t i = 0; i < closed.size(); ++i) {
        terminal[i] = take(poller.add(closed[i], Interest::state));
        require(std::holds_alternative<Success>(closed[i].close(AbortClose{})), "terminal close");
    }
    auto readable = take(poller.add(*pair.rx, Interest::read));
    std::array<std::byte, 1> byte{std::byte{42}};
    require(std::holds_alternative<size_t>(pair.tx.write_stream(byte)), "fairness send");
    // Independently wait for native readability before checking the mixed poller.
    auto probe = take(pair.sp.add(*pair.rx, Interest::read));
    ready(pair.sp, probe, Interest::read);
    std::array<Event, 4> batch{};
    auto n = poller.wait(batch, InfiniteWait{});
    require(std::holds_alternative<size_t>(n), "mixed wait failed");
    bool saw_read = false;
    for (size_t i = 0; i < std::get<size_t>(n); ++i)
        saw_read |= batch[i].registration.id == readable.id;
    require(saw_read, "terminal readiness hides native readiness with spare capacity");
    // With one slot, both readiness classes and every terminal must make progress.
    std::array<Event, 1> single{};
    std::array<bool, 3> seen{};
    saw_read = false;
    for (size_t i = 0; i < 12; ++i) {
        auto result = poller.wait(single, InfiniteWait{});
        require(std::holds_alternative<size_t>(result) && std::get<size_t>(result) == 1, "single-slot wait");
        saw_read |= single[0].registration.id == readable.id;
        for (size_t j = 0; j < seen.size(); ++j) seen[j] = seen[j] || single[0].registration.id == terminal[j].id;
    }
    require(saw_read && seen[0] && seen[1] && seen[2], "bounded output starves ready registration");
    take(poller.wake());
    require(std::holds_alternative<Interrupted>(poller.wait(single, InfiniteWait{})), "wake must precede mixed readiness");
    require(std::holds_alternative<size_t>(poller.wait(single, 0ms)), "wake discarded readiness");
    require(std::holds_alternative<size_t>(pair.rx->read_stream(byte)), "fairness read");
}
void registration_lifetimes(Provider& provider) {
    auto poller = take(provider.create_poller());
    auto other = take(provider.create_poller());
    for (int i = 0; i < 512; ++i) {
        Registration token{}, second{};
        {
            auto connection = take(provider.create_connection(live_profile()));
            token = take(poller.add(connection, Interest::read));
            second = take(other.add(connection, Interest::read));
            require(std::holds_alternative<Error>(poller.add(connection, Interest::read)), "duplicate registration");
            auto moved = std::move(connection);
            take(poller.update(token, Interest::state));
        }
        require(std::holds_alternative<Error>(poller.update(token, Interest::read)), "destroyed object registration retained");
        require(std::holds_alternative<Error>(poller.remove(token)), "destroyed object token still registered");
        require(std::holds_alternative<Error>(other.remove(second)), "second poller retained object");
    }
    for (int i = 0; i < 32; ++i) {
        std::future<Result<Success>> removal;
        {
            auto connection = take(provider.create_connection(live_profile()));
            require(std::holds_alternative<Error>(poller.add(connection, static_cast<Interest>(0))), "invalid interests accepted");
            auto token = take(poller.add(connection, Interest::read)); // Failed add rolled back both links.
            removal = std::async(std::launch::async, [&poller, token] { return poller.remove(token); });
        } // Removal may race object destruction, without accessing its wrapper.
        (void)removal.get();
    }
    Registration listener_token{};
    {
        auto listener = take(provider.listen({{"127.0.0.1", 0, 0}, 8, live_profile(), {}}));
        listener_token = take(poller.add(listener, Interest::read));
    }
    require(std::holds_alternative<Error>(poller.remove(listener_token)), "destroyed listener registration retained");
    auto listener = take(provider.listen({{"127.0.0.1", 0, 0}, 8, live_profile(), {}}));
    {
        auto short_lived = take(provider.create_poller());
        auto token = take(short_lived.add(listener, Interest::read));
        take(short_lived.remove(token));
        take(short_lived.add(listener, Interest::read));
        auto moved = std::move(short_lived);
    } // Poller can also disappear before the registered object.
    auto token = take(poller.add(listener, Interest::read));
    take(poller.remove(token));
    std::array<Event, 1> event{};
    require(std::holds_alternative<TimedOut>(poller.wait(event, 0ms)), "released registrations left events");
}
int main(int argc,char** argv) {
    try {
        require(argc==4,"two providers and load order required");
        auto runtime=take(TransportRuntime::create());
        bool reversed=std::string(argv[3])=="reversed";
        auto first=take(runtime.load({reversed?argv[2]:argv[1],reversed?"haivision":"robotweax",reversed?"1.5.7":"0.3.5",{}}));
        auto second=take(runtime.load({reversed?argv[1]:argv[2],reversed?"robotweax":"haivision",reversed?"0.3.5":"1.5.7",{}}));
        auto copy=first;
        require(copy.info().build_fingerprint==first.info().build_fingerprint,"provider shared ownership");
        auto again=take(runtime.load({argv[1],"robotweax","0.3.5",{}}));
        require(take(again.now()).clock_domain==take((reversed?second:first).now()).clock_domain,"duplicate runtime startup");
        terminal_fairness(first); terminal_fairness(second);
        registration_lifetimes(first); registration_lifetimes(second);
        cancellation(first); cancellation(second);
        polling_batches(first,second); polling_batches(second,first);
        for(bool file:{false,true}) for(bool encrypted:{false,true}) {
            transfer(first,second,file,encrypted); transfer(second,first,file,encrypted);
        }
        for(bool file:{false,true}) for(bool encrypted:{false,true}) {
            transfer(first,second,file,encrypted,true); transfer(second,first,file,encrypted,true);
        }
        auto profile = take(first.create_profile(ProfileConfig::basic(live_profile())));
        require(std::holds_alternative<Error>(second.listen_profile(profile, {"127.0.0.1",0,0})), "foreign facade profile");
        auto concurrent=std::async(std::launch::async,[&]{ transfer(first,second,true,false); });
        transfer(second,first,true,false); concurrent.get();
        auto p=take(first.create_poller()), q=take(second.create_poller());
        std::array<Event,1> a{},b{};
        auto one=std::async(std::launch::async,[&]{ return p.wait(a,InfiniteWait{}); });
        auto two=std::async(std::launch::async,[&]{ return q.wait(b,InfiniteWait{}); });
        take(runtime.begin_shutdown());
        require(std::holds_alternative<Interrupted>(one.get()) && std::holds_alternative<Interrupted>(two.get()),"shutdown cancellation");
        require(std::holds_alternative<Success>(runtime.shutdown_status()),"shutdown pending");
        require(std::holds_alternative<Error>(first.create_connection(live_profile())),"creation after shutdown");
        std::cout<<"Facade polling, cross-provider transfers, concurrent close and shutdown passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
