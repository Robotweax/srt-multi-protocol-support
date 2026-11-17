#include <srtmux/module.hpp>

#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace srtmux::host;
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
ModuleSpec spec(const char* path, const char* name = "fixture") {
    return {std::filesystem::path(path), name, "test-1", "fixture-build"};
}
Module load(const ModuleSpec& specification) {
    auto result = Module::load(specification);
    if (auto* error = std::get_if<LoadError>(&result))
        throw std::runtime_error(error->message);
    return std::get<Module>(std::move(result));
}
void fails(const ModuleSpec& specification, LoadErrorCode expected) {
    auto result = Module::load(specification);
    require(std::holds_alternative<LoadError>(result), "Invalid module unexpectedly loaded");
    require(std::get<LoadError>(result).code == expected, "Wrong load failure category");
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 12, "Expected order and ten fixture paths");
        const auto a_spec = spec(argv[2], "fixture-a");
        const auto b_spec = spec(argv[3], "fixture-b");
        if (std::string(argv[1]) == "reversed") (void)load(b_spec);
        auto a = load(a_spec);
        auto b = load(b_spec);
        require(a.info().implementation == "fixture-a", "Wrong first identity");
        require(b.info().implementation == "fixture-b", "Wrong second identity");
        require(&a.api() != &b.api(), "Distinct modules share a function table");
        require(a.api().clock_now_us != b.api().clock_now_us, "Distinct modules share dispatch");

        // Repeated and concurrent loads share the same validated immutable table.
        auto again = load(a_spec);
        require(&again.api() == &a.api(), "Repeated load did not reuse module");
        std::array<const srtm_provider_api_v1*, 12> tables{};
        std::vector<std::thread> threads;
        for (std::size_t i = 0; i < tables.size(); ++i)
            threads.emplace_back([&, i] { tables[i] = &load(a_spec).api(); });
        for (auto& thread : threads) thread.join();
        for (auto* table : tables) require(table == &a.api(), "Concurrent load changed module");

        // State and colliding internal symbols must remain isolated in both orders.
        srtm_runtime *ar = nullptr, *br = nullptr, *duplicate = nullptr;
        srtm_error_v1 error{};
        error.struct_size = sizeof(error);
        srtm_host_v1 host{};
        host.struct_size = sizeof(host);
        require(a.api().runtime_create(&host, &ar, &error) == SRTM_OK, "First runtime create failed");
        require(b.api().runtime_create(&host, &br, &error) == SRTM_OK, "Second runtime create failed");
        require(a.api().runtime_create(&host, &duplicate, &error) == SRTM_FAILED, "Duplicate runtime accepted");
        require(error.category == SRTM_ERR_BUSY, "Duplicate runtime not classified Busy");
        uint64_t value = 0;
        require(a.api().clock_now_us(ar, &value, &error) == SRTM_OK && value == 11, "A resolved B's internal symbol");
        require(b.api().clock_now_us(br, &value, &error) == SRTM_OK && value == 22, "B resolved A's internal symbol");
        require(a.api().runtime_begin_shutdown(ar, &error) == SRTM_OK, "A shutdown failed");
        require(a.api().runtime_destroy(ar, &error) == SRTM_OK, "A destroy failed");
        require(b.api().runtime_shutdown_status(br, &error) == SRTM_PENDING, "A shutdown changed B state");
        require(b.api().clock_now_us(br, &value, &error) == SRTM_OK && value == 22, "B failed after A shutdown");
        require(b.api().runtime_begin_shutdown(br, &error) == SRTM_OK, "B shutdown failed");
        require(b.api().runtime_destroy(br, &error) == SRTM_OK, "B destroy failed");

        fails(spec(argv[4]), LoadErrorCode::invalid_table);
        fails(spec(argv[5]), LoadErrorCode::invalid_table);
        fails(spec(argv[6]), LoadErrorCode::incompatible_abi);
        fails(spec(argv[7]), LoadErrorCode::invalid_descriptor);
        fails(spec(argv[7]), LoadErrorCode::invalid_descriptor); // Rejection is stable on retry.
        fails(spec(argv[8]), LoadErrorCode::provider_failed);
        fails(spec(argv[9]), LoadErrorCode::invalid_table);
        require(load(spec(argv[10])).api().abi_minor == 12, "Compatible future minor rejected");
        fails(spec(argv[11]), LoadErrorCode::missing_entry);
        fails(spec("relative-module"), LoadErrorCode::invalid_spec);
        auto nonexistent = a_spec;
        nonexistent.path += ".absent";
        fails(nonexistent, LoadErrorCode::open_failed);
        auto mismatch = a_spec;
        mismatch.release = "wrong";
        fails(mismatch, LoadErrorCode::identity_mismatch);
        mismatch = a_spec;
        mismatch.build_fingerprint = "wrong";
        fails(mismatch, LoadErrorCode::identity_mismatch);

        // Pinning outlives every public Module value; table remains callable.
        const auto* pinned = [&] { auto temporary = load(spec(argv[10])); return &temporary.api(); }();
        require(pinned->clock_now_us(nullptr, &value, &error) == SRTM_OK, "Module was unloaded prematurely");
        std::cout << "Module isolation, validation and lifetime checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
