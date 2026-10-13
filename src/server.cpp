// Copyright 2026 Sendspin Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "sendspin/server/server.h"

#include "audio_workers.h"
#include "client_records.h"
#include "clock.h"
#include "crypto/crypto.h"
#include "crypto/keys.h"
#include "crypto/pairing_token.h"
#include "discovery_policy.h"
#include "dispatcher.h"
#include "fragmentation.h"
#include "host_view.h"
#include "image_codec.h"
#include "log.h"
#include "messages.h"
#include "pairing.h"
#include "playback_policy.h"
#include "sched/constants.h"
#include "server_core.h"
#include "thread_setup.h"
#include "transport.h"
#include <noise/protocol.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace sendspin::server {

namespace {

/// How long stop() lets closing handshakes in progress finish before the core thread exits
constexpr int64_t SHUTDOWN_GRACE_US = 1000000;

/// A routed source's input format until the source's first stream sets it
constexpr InputFormat ROUTED_SOURCE_FORMAT{48000, 2, SampleFormat::S16};

std::once_flag noise_init_once;

std::atomic<uint64_t> next_input_id{1};

/// The stored server key, or a new one saved and committed; nullopt when the provider fails or
/// holds a blob that is not a valid key
std::optional<Identity> load_or_create_identity(PersistenceProvider& provider, const Logger& log) {
    std::optional<std::vector<uint8_t>> blob = provider.load_blob(persistence_keys::SERVER_KEY);
    if (blob) {
        std::optional<Identity> identity;
        if (blob->size() == persistence_keys::SERVER_KEY_SIZE) {
            std::array<uint8_t, X25519_KEY_SIZE> private_bytes{};
            std::copy(blob->begin(), blob->end(), private_bytes.begin());
            identity = Identity::from_private_bytes(private_bytes);
            secure_zero(private_bytes.data(), private_bytes.size());
        }
        secure_zero(blob->data(), blob->size());
        if (!identity) {
            SS_LOGE(log, "the stored server key is not a valid key");
        }
        return identity;
    }
    std::optional<Identity> identity = Identity::generate();
    if (!identity) {
        SS_LOGE(log, "generating the server key failed");
        return std::nullopt;
    }
    if (!provider.save_blob(persistence_keys::SERVER_KEY, identity->private_bytes.data(),
                            identity->private_bytes.size()) ||
        !provider.commit()) {
        SS_LOGE(log, "saving the new server key failed");
        return std::nullopt;
    }
    return identity;
}

/// Whether every value is within its documented range (config.h)
bool is_valid(const ServerConfig& config) {
    const ServerTiming& timing = config.timing;
    const auto in_range = [](int64_t us) { return us > 0 && us <= config_limits::MAX_TIMING_US; };
    if (config.name.size() > config_limits::MAX_NAME_BYTES ||
        config.languages.size() > config_limits::MAX_LANGUAGES ||
        !in_range(timing.handshake_step_timeout_us) || !in_range(timing.write_stall_us) ||
        !in_range(timing.handshake_backoff_min_us) || !in_range(timing.handshake_backoff_max_us) ||
        !in_range(timing.group_rejoin_grace_us) || !in_range(timing.pairing_start_timeout_us) ||
        timing.handshake_backoff_max_us < timing.handshake_backoff_min_us ||
        config.resampler_quality > ResamplerQuality::VERY_HIGH ||
        config.log_level > LogLevel::INFO ||
        !is_valid_path(config.path, config_limits::MAX_PATH_BYTES)) {
        return false;
    }
    return std::all_of(config.languages.begin(), config.languages.end(),
                       [](const std::string& language) {
                           return language.size() <= config_limits::MAX_LANGUAGE_TAG_BYTES;
                       });
}

/// Whether options name a method and, for a dynamic code only, a format the operator can enter
bool is_valid_pairing(const PairingOptions& options, bool operator_can_scan_qr) {
    if (options.method > PairMethod::DYNAMIC_PAIRING_CODE) {
        return false;
    }
    if (!options.format) {
        return true;
    }
    return options.method == PairMethod::DYNAMIC_PAIRING_CODE &&
           (*options.format == CodeFormat::DIGITS ||
            (*options.format == CodeFormat::QR_CODE && operator_can_scan_qr));
}

/// Text that wipes itself, for a pairing code on its way to the core thread
struct WipedText {
    explicit WipedText(std::string_view input) : text(input) {}
    ~WipedText() {
        secure_zero(this->text.data(), this->text.size());
    }
    WipedText(const WipedText&) = delete;
    WipedText& operator=(const WipedText&) = delete;

    std::string text;
};

CoreSettings core_settings(const ServerConfig& config) {
    CoreSettings settings;
    settings.session.hello = ServerHello{config.name, config.languages, {Codec::PCM, Codec::FLAC}};
    // roles/source/v1.md "client/hello source@v1 support object": a server MAY accept `opus`
    if (config.enable_opus && source_opus_available()) {
        settings.session.hello.source_codecs.push_back(Codec::OPUS);
    }
    settings.session.step_timeout_us = config.timing.handshake_step_timeout_us;
    settings.session.max_fragment_plaintext =
        std::clamp(config.max_message_bytes, MIN_FRAGMENT_PLAINTEXT, MAX_TRANSPORT_PLAINTEXT);
    settings.transport.write_stall_us = config.timing.write_stall_us;
    settings.transport.path = config.path;
    settings.backoff = {config.timing.handshake_backoff_min_us,
                        config.timing.handshake_backoff_max_us};
    settings.trace = config.trace;
    settings.trace_writes = config.trace_writes;
    settings.approve_all_clients = config.approve_all_clients;
    settings.group_rejoin_grace_us = config.timing.group_rejoin_grace_us;
    settings.pairing_start_timeout_us = config.timing.pairing_start_timeout_us;
    settings.operator_can_scan_qr = config.operator_can_scan_qr;
    settings.group_name = config.name;
    settings.resampler_quality = config.resampler_quality;
    settings.enable_opus = config.enable_opus;
    settings.log = config.log;
    settings.log_level = config.log_level;
    return settings;
}

/// Posts browse results to the core thread; one that arrives after the loop ended is dropped
class CoreSink : public DiscoverySink {
public:
    CoreSink(EventLoop& loop, ServerCore*& core) : loop_(loop), core_(core) {}

    void on_client_found(const DiscoveredClient& client) override {
        this->loop_.post([core = this->core_, client = DiscoveredClient(client)] {
            core->on_client_found(client);
        });
    }

    void on_client_lost(const std::string& instance) override {
        this->loop_.post([core = this->core_, instance = std::string(instance)] {
            core->on_client_lost(instance);
        });
    }

    void on_unavailable() override {
        this->loop_.post([core = this->core_] { core->on_discovery_unavailable(); });
    }

    void on_available() override {
        this->loop_.post([core = this->core_] { core->on_discovery_available(); });
    }

private:
    EventLoop& loop_;
    /// Set before the provider starts
    ServerCore*& core_;
};

}  // namespace

/// One run, from start() to the join after stop(). The core thread destroys the ServerCore when
/// its loop ends; everything else is immutable while the threads run, except what carries its
/// own lock.
struct Running {
    Running(const ServerConfig& config, Identity identity_in, ServerListener* listener_in)
        : settings(core_settings(config)),
          identity(std::move(identity_in)),
          server_id(identity.peer_id()),
          listener(listener_in) {}

    ~Running() {
        this->join();
    }

    Running(const Running&) = delete;
    Running& operator=(const Running&) = delete;

    /// Waits for the run's threads; never from one of them. The group audio threads stop once
    /// core has, and before the loop their jobs post to goes. The dispatcher is told to finish
    /// here too, for a run whose core thread never started.
    void join() {
        if (this->core_thread.joinable()) {
            this->core_thread.join();
        }
        this->workers.stop();
        if (this->dispatcher) {
            this->dispatcher->finish();
            this->dispatcher->join();
        }
    }

    const CoreSettings settings;
    const Identity identity;
    const std::string server_id;
    ServerListener* const listener;
    ClientSnapshot snapshot;
    GroupSnapshot groups;
    /// The inputs open_input() made whose attachment has not run
    InputRegistry inputs;
    /// The input each group holds on the host side
    GroupInputs group_inputs;
    /// Set before the core thread starts
    GroupId default_group;
    /// Group ids, taken by create_group() on host threads and by core for the groups it makes
    std::atomic<uint64_t> group_ids{1};
    /// Shared with the inputs, which may outlive the run
    std::shared_ptr<Dispatcher> dispatcher;
    EventLoop loop;
    /// The group audio threads: declared after the loop, so they are joined before it goes
    AudioWorkers workers{AudioWorkers::default_threads()};
    std::unique_ptr<ServerCore> core_owner;
    /// Posted to from any thread; a handler posted after the loop ended is never run
    ServerCore* core{nullptr};
    /// The bound ServerConfig::listen_port
    uint16_t listen_port{0};
    CoreSink sink{loop, core};
    /// Started before the core thread, stopped on it as the run stops; nullptr when neither
    /// advertising nor browsing
    std::shared_ptr<DiscoveryProvider> discovery;
    std::thread core_thread;
};

/// The input's host view, which core holds too
class GroupInput::Impl {
public:
    explicit Impl(std::shared_ptr<HostView> view) : view(std::move(view)) {}

    const std::shared_ptr<HostView> view;
};

class SendspinServer::Impl {
public:
    /// Stops the run and joins every run's threads; from the server's destructor, while a listener
    /// call in progress may still call the server's methods
    void shut_down() {
        try {
            this->stop();
            this->join_stopped();
        } catch (...) {
            // Threads that could not be stopped or joined would outlive the server
            std::terminate();
        }
    }

    StartResult start(const ServerConfig& config, PersistenceProvider* provider,
                      ServerListener* listener) {
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            if (this->running_ || this->starting_) {
                return StartResult::ALREADY_RUNNING;
            }
            this->starting_ = true;
        }
        StartResult result = StartResult::PERSISTENCE_FAILED;
        try {
            this->join_stopped();
            result = this->start_run(config, provider, listener);
        } catch (...) {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            this->starting_ = false;
            throw;
        }
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->starting_ = false;
        return result;
    }

    void stop() {
        std::shared_ptr<Running> running;
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            running = std::exchange(this->running_, nullptr);
            if (running) {
                this->stopped_.push_back(running);
            }
        }
        if (running) {
            running->loop.post([run = running.get()] {
                // No browse result reaches the core after its shutdown
                if (run->discovery) {
                    run->discovery->stop();
                }
                run->core->shutdown(SHUTDOWN_GRACE_US);
            });
        }
    }

    std::string server_id() const {
        const std::shared_ptr<Running> running = this->running();
        return running ? running->server_id : std::string{};
    }

    uint16_t listen_port() const {
        const std::shared_ptr<Running> running = this->running();
        return running ? running->listen_port : 0;
    }

    std::vector<ClientInfo> clients() const {
        const std::shared_ptr<Running> running = this->running();
        return running ? running->snapshot.read() : std::vector<ClientInfo>{};
    }

    bool connect(std::string_view client_id) {
        const std::shared_ptr<Running> running = this->running();
        if (!running) {
            return false;
        }
        const std::vector<ClientInfo> clients = running->snapshot.read();
        if (client_id.empty() ||
            std::none_of(clients.begin(), clients.end(), [client_id](const ClientInfo& client) {
                return client.client_id == client_id;
            })) {
            return false;
        }
        running->loop.post(
            [core = running->core, id = std::string(client_id)] { core->connect(id); });
        return true;
    }

    bool connect_url(std::string_view url, const ConnectOptions& options) {
        std::optional<DialTarget> target = parse_ws_url(url);
        const std::shared_ptr<Running> running = this->running();
        if (!target || !running) {
            return false;
        }
        running->loop.post(
            [core = running->core, url = std::string(url), target = std::move(*target),
             retry = options.retry_until_connected] { core->connect_url(url, target, retry); });
        return true;
    }

    bool cancel_connect_url(std::string_view url) {
        const std::shared_ptr<Running> running = this->running();
        if (!parse_ws_url(url) || !running) {
            return false;
        }
        running->loop.post(
            [core = running->core, url = std::string(url)] { core->cancel_connect_url(url); });
        return true;
    }

    bool disconnect(std::string_view client_id) {
        const std::shared_ptr<Running> running = this->running();
        if (!running) {
            return false;
        }
        const std::vector<ClientInfo> clients = running->snapshot.read();
        if (client_id.empty() ||
            std::none_of(clients.begin(), clients.end(), [client_id](const ClientInfo& client) {
                return client.connected && client.client_id == client_id;
            })) {
            return false;
        }
        running->loop.post(
            [core = running->core, id = std::string(client_id)] { core->disconnect(id); });
        return true;
    }

    std::vector<GroupInfo> groups() const {
        const std::shared_ptr<Running> running = this->running();
        return running ? running->groups.read() : std::vector<GroupInfo>{};
    }

    std::optional<GroupId> default_group() const {
        const std::shared_ptr<Running> running = this->running();
        return running ? std::optional<GroupId>(running->default_group) : std::nullopt;
    }

    /// @param min_lead_us a live input's least lead (LiveOptions)
    OpenedInput open_input(GroupId group, const InputOptions& options, int64_t min_lead_us = 0) {
        const std::shared_ptr<Running> running = this->running();
        if (!running) {
            return {OpenInputResult::NOT_RUNNING, nullptr};
        }
        if (!is_valid_input(options.format)) {
            return {OpenInputResult::INVALID_FORMAT, nullptr};
        }
        if (options.kind == InputKind::LIVE &&
            (options.write_jitter_us < 0 || options.write_jitter_us > MAX_WRITE_JITTER_US)) {
            return {OpenInputResult::INVALID_JITTER, nullptr};
        }
        const std::vector<GroupInfo> groups = running->groups.read();
        if (std::none_of(groups.begin(), groups.end(),
                         [group](const GroupInfo& info) { return info.id == group; })) {
            return {OpenInputResult::UNKNOWN_GROUP, nullptr};
        }
        // Checked before the view and its staging ring are built; the claim below decides a race
        if (!running->group_inputs.available(group)) {
            return {OpenInputResult::GROUP_BUSY, nullptr};
        }
        // No write-ahead until core attaches the input and writes its group's parameters, so the
        // first write anchors with the group's own lead
        const sched::SchedTiming defaults;
        const GroupParams initial{defaults.group_default_us, 0, 0};
        auto view = std::make_shared<HostView>(
            group, InputId{next_input_id.fetch_add(1)}, options, initial, running->dispatcher,
            running->listener, LiveOptions{min_lead_us, running->settings.resampler_quality});
        if (!running->group_inputs.claim(group, view)) {
            return {OpenInputResult::GROUP_BUSY, nullptr};
        }
        // Refused once the run is stopping; a registered view is closed by the attach or the stop
        if (!running->inputs.add(view)) {
            view->close();
            return {OpenInputResult::NOT_RUNNING, nullptr};
        }
        running->loop.post([core = running->core, view] { core->attach_input(view); });
        return {OpenInputResult::OK, std::shared_ptr<GroupInput>(new GroupInput(
                                         std::make_unique<GroupInput::Impl>(std::move(view))))};
    }

    RoutedSource route_source(std::string_view client_id, GroupId group,
                              const SourceRouteOptions& options) {
        if (!public_key_from_peer_id(client_id)) {
            return {this->running() ? RouteSourceResult::INVALID_CLIENT
                                    : RouteSourceResult::NOT_RUNNING,
                    nullptr};
        }
        if (options.write_jitter_us < 0 || options.write_jitter_us > MAX_WRITE_JITTER_US) {
            return {RouteSourceResult::INVALID_JITTER, nullptr};
        }
        if (options.min_latency_us < 0 || options.min_latency_us > MAX_SOURCE_MIN_LATENCY_US) {
            return {RouteSourceResult::INVALID_LATENCY, nullptr};
        }
        InputOptions input;
        input.format = ROUTED_SOURCE_FORMAT;
        input.kind = InputKind::LIVE;
        input.write_jitter_us = options.write_jitter_us;
        OpenedInput opened = this->open_input(group, input, options.min_latency_us);
        switch (opened.result) {
            case OpenInputResult::OK:
                break;
            case OpenInputResult::UNKNOWN_GROUP:
                return {RouteSourceResult::UNKNOWN_GROUP, nullptr};
            case OpenInputResult::GROUP_BUSY:
                return {RouteSourceResult::GROUP_BUSY, nullptr};
            case OpenInputResult::NOT_RUNNING:
            case OpenInputResult::INVALID_FORMAT:
            case OpenInputResult::INVALID_JITTER:
                return {RouteSourceResult::NOT_RUNNING, nullptr};
        }
        const std::shared_ptr<Running> running = this->running();
        if (!running) {
            return {RouteSourceResult::NOT_RUNNING, nullptr};
        }
        // Posted behind the input's attachment
        running->loop.post(
            [core = running->core, id = std::string(client_id), view = opened.input->impl_->view,
             jitter = options.write_jitter_us] { core->route_source(id, view, jitter); });
        return {RouteSourceResult::OK, std::move(opened.input)};
    }

    bool approve(std::string_view client_id, std::optional<ApprovalScope> approval) {
        const std::shared_ptr<Running> running = this->running();
        if (!running || !public_key_from_peer_id(client_id)) {
            return false;
        }
        running->loop.post([core = running->core, id = std::string(client_id), approval] {
            core->set_approval(id, approval);
        });
        return true;
    }

    bool set_player_format(std::string_view client_id, std::optional<StreamFormat> format) {
        const std::shared_ptr<Running> running = this->running();
        if (!running || !public_key_from_peer_id(client_id)) {
            return false;
        }
        std::optional<AudioFormat> preferred;
        if (format) {
            preferred = client_format(*format);
        }
        running->loop.post([core = running->core, id = std::string(client_id), preferred] {
            core->set_player_format(id, preferred);
        });
        return true;
    }

    bool begin_pairing(std::string_view client_id, const PairingOptions& options) {
        const std::shared_ptr<Running> running = this->running();
        if (!running || !is_valid_pairing(options, running->settings.operator_can_scan_qr)) {
            return false;
        }
        const std::optional<ClientInfo> client = listed(*running, client_id);
        // A connected client's offer is known; a client dialed for pairing is checked on core
        if (!client ||
            (client->connected && !offers_method(client->pairing_offers, options.method))) {
            return false;
        }
        // The format must be one the connected client offers and the operator can use
        if (client->connected && options.method == PairMethod::DYNAMIC_PAIRING_CODE &&
            !dynamic_code_format(client->pairing_offers, options.format,
                                 running->settings.operator_can_scan_qr)) {
            return false;
        }
        running->loop.post([core = running->core, id = std::string(client_id), options] {
            core->begin_pairing(id, options);
        });
        return true;
    }

    bool submit_pairing_token(std::string_view client_id, std::string_view token) {
        const std::shared_ptr<Running> running = this->running();
        if (!running || !listed(*running, client_id)) {
            return false;
        }
        // pairing.md "Pairing Token": the client key must be the connection's client_id
        std::optional<PairingPskToken> decoded = decode_pairing_psk_token(token);
        const std::optional<PublicKey> key = public_key_from_peer_id(client_id);
        if (!decoded || !key || decoded->client_key != *key) {
            return false;
        }
        auto held = std::make_shared<const PairingPskToken>(std::move(*decoded));
        running->loop.post([core = running->core, id = std::string(client_id), held] {
            core->submit_pairing_token(id, *held);
        });
        return true;
    }

    bool submit_pairing_code(std::string_view client_id, std::string_view code) {
        const std::shared_ptr<Running> running = this->running();
        if (!running || !public_key_from_peer_id(client_id)) {
            return false;
        }
        auto secret = std::make_shared<WipedText>(code);
        running->loop.post([core = running->core, id = std::string(client_id), secret] {
            core->submit_pairing_code(id, secret->text);
        });
        return true;
    }

    bool cancel_pairing(std::string_view client_id) {
        const std::shared_ptr<Running> running = this->running();
        if (!running || !public_key_from_peer_id(client_id)) {
            return false;
        }
        running->loop.post(
            [core = running->core, id = std::string(client_id)] { core->cancel_pairing(id); });
        return true;
    }

    bool unpair(std::string_view client_id) {
        const std::shared_ptr<Running> running = this->running();
        if (!running || !public_key_from_peer_id(client_id)) {
            return false;
        }
        running->loop.post(
            [core = running->core, id = std::string(client_id)] { core->unpair(id); });
        return true;
    }

    std::optional<GroupId> create_group(std::string_view name, std::string_view first_member) {
        const std::shared_ptr<Running> running = this->running();
        if (!running || name.size() > config_limits::MAX_NAME_BYTES ||
            !public_key_from_peer_id(first_member)) {
            return std::nullopt;
        }
        const GroupId id{running->group_ids.fetch_add(1)};
        running->loop.post(
            [core = running->core, id, name = std::string(name),
             member = std::string(first_member)] { core->create_group(id, name, member); });
        return id;
    }

    /// The run, when group is in its groups(); nullptr while stopped or for another group
    std::shared_ptr<Running> running_with(GroupId group) const {
        std::shared_ptr<Running> running = this->running();
        if (!running) {
            return nullptr;
        }
        const std::vector<GroupInfo> groups = running->groups.read();
        if (std::none_of(groups.begin(), groups.end(),
                         [group](const GroupInfo& info) { return info.id == group; })) {
            return nullptr;
        }
        return running;
    }

private:
    StartResult start_run(const ServerConfig& config, PersistenceProvider* provider,
                          ServerListener* listener) {
        if (!is_valid(config)) {
            return StartResult::INVALID_CONFIG;
        }
        // Before any thread uses noise-c
        std::call_once(noise_init_once, [] { noise_init_framework(); });

        // The provider is called on the dispatch thread only, so the load runs there too
        auto dispatcher = std::make_shared<Dispatcher>();
        const Logger log(*dispatcher, config.log, config.log_level);
        std::promise<std::optional<Identity>> loaded;
        ClientRecords records;
        dispatcher->post([provider, &loaded, &records, &log] {
            if (provider == nullptr) {
                SS_LOGE(log, "no persistence provider to load the server key from");
                loaded.set_value(std::nullopt);
                return;
            }
            std::optional<Identity> identity = load_or_create_identity(*provider, log);
            if (identity) {
                records = load_client_records(*provider, log);
            }
            loaded.set_value(std::move(identity));
        });
        std::optional<Identity> identity = loaded.get_future().get();
        if (!identity) {
            return StartResult::PERSISTENCE_FAILED;
        }

        auto running = std::make_shared<Running>(config, std::move(*identity), listener);
        running->dispatcher = std::move(dispatcher);
        running->core_owner = std::make_unique<ServerCore>(
            running->loop, running->workers, *running->dispatcher, listener, running->identity,
            running->settings, running->snapshot, running->groups, running->inputs,
            running->group_ids, provider, std::move(records));
        running->core = running->core_owner.get();
        running->default_group = running->core->default_group();
        if (!running->core->listen(config.listen_port)) {
            return StartResult::BIND_FAILED;
        }
        running->listen_port = running->core->listen_port();
        if (config.advertise || config.browse) {
            std::shared_ptr<DiscoveryProvider> provider =
                config.discovery ? config.discovery : platform_discovery();
            DiscoveryRequest request;
            if (config.advertise) {
                request.advertise =
                    advertised_server(config.name, running->listen_port, config.path);
            }
            request.browse = config.browse;
            if (provider && provider->start(request, running->sink)) {
                running->discovery = std::move(provider);
            } else if (config.discovery) {
                return StartResult::DISCOVERY_FAILED;
            } else {
                // The server runs without mDNS, reached by URL and by clients that know its
                // address
                running->sink.on_unavailable();
            }
        }
        running->core_thread = start_thread(ThreadRole::CORE, [run = running.get()] {
            run->loop.run();
            run->core_owner.reset();
        });
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->running_ = std::move(running);
        return StartResult::OK;
    }

    /// The client clients() lists with client_id; nullopt for none or an empty client_id
    static std::optional<ClientInfo> listed(const Running& running, std::string_view client_id) {
        if (client_id.empty()) {
            return std::nullopt;
        }
        for (ClientInfo& client : running.snapshot.read()) {
            if (client.client_id == client_id) {
                return std::move(client);
            }
        }
        return std::nullopt;
    }

    std::shared_ptr<Running> running() const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return this->running_;
    }

    /// Joins the runs stop() ended; from start() and the destructor, never a listener
    void join_stopped() {
        std::vector<std::shared_ptr<Running>> stopped;
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            stopped.swap(this->stopped_);
        }
        for (const std::shared_ptr<Running>& running : stopped) {
            running->join();
        }
    }

    /// Guards only the members below; held to copy or swap them
    mutable std::mutex mutex_;
    std::shared_ptr<Running> running_;
    std::vector<std::shared_ptr<Running>> stopped_;
    bool starting_{false};
};

SendspinServer::SendspinServer() : impl_(std::make_unique<Impl>()) {}

SendspinServer::~SendspinServer() {
    this->impl_->shut_down();
}

StartResult SendspinServer::start(const ServerConfig& config, PersistenceProvider* provider,
                                  ServerListener* listener) {
    return this->impl_->start(config, provider, listener);
}

void SendspinServer::stop() {
    this->impl_->stop();
}

int64_t SendspinServer::now_us() const {
    return ::sendspin::server::now_us();
}

std::string SendspinServer::server_id() const {
    return this->impl_->server_id();
}

uint16_t SendspinServer::listen_port() const {
    return this->impl_->listen_port();
}

std::vector<ClientInfo> SendspinServer::clients() const {
    return this->impl_->clients();
}

bool SendspinServer::connect_url(std::string_view url, const ConnectOptions& options) {
    return this->impl_->connect_url(url, options);
}

bool SendspinServer::cancel_connect_url(std::string_view url) {
    return this->impl_->cancel_connect_url(url);
}

bool SendspinServer::disconnect(std::string_view client_id) {
    return this->impl_->disconnect(client_id);
}

bool SendspinServer::connect(std::string_view client_id) {
    return this->impl_->connect(client_id);
}

std::vector<GroupInfo> SendspinServer::groups() const {
    return this->impl_->groups();
}

std::optional<GroupId> SendspinServer::default_group() const {
    return this->impl_->default_group();
}

OpenedInput SendspinServer::open_input(GroupId group, const InputOptions& options) {
    return this->impl_->open_input(group, options);
}

RoutedSource SendspinServer::route_source(std::string_view client_id, GroupId group,
                                          const SourceRouteOptions& options) {
    return this->impl_->route_source(client_id, group, options);
}

bool SendspinServer::approve(std::string_view client_id, ApprovalScope scope) {
    return this->impl_->approve(client_id, scope);
}

bool SendspinServer::revoke_approval(std::string_view client_id) {
    return this->impl_->approve(client_id, std::nullopt);
}

bool SendspinServer::begin_pairing(std::string_view client_id, const PairingOptions& options) {
    return this->impl_->begin_pairing(client_id, options);
}

bool SendspinServer::submit_pairing_token(std::string_view client_id, std::string_view token) {
    return this->impl_->submit_pairing_token(client_id, token);
}

bool SendspinServer::submit_pairing_code(std::string_view client_id, std::string_view code) {
    return this->impl_->submit_pairing_code(client_id, code);
}

bool SendspinServer::cancel_pairing(std::string_view client_id) {
    return this->impl_->cancel_pairing(client_id);
}

bool SendspinServer::unpair(std::string_view client_id) {
    return this->impl_->unpair(client_id);
}

std::optional<GroupId> SendspinServer::create_group(std::string_view name,
                                                    std::string_view first_member) {
    return this->impl_->create_group(name, first_member);
}

bool SendspinServer::rename_group(GroupId group, std::string_view name) {
    const std::shared_ptr<Running> running = this->impl_->running_with(group);
    if (!running || name.size() > config_limits::MAX_NAME_BYTES) {
        return false;
    }
    running->loop.post([core = running->core, group, name = std::string(name)] {
        core->rename_group(group, name);
    });
    return true;
}

bool SendspinServer::move_client(std::string_view client_id, GroupId group) {
    const std::shared_ptr<Running> running = this->impl_->running_with(group);
    if (!running || !public_key_from_peer_id(client_id)) {
        return false;
    }
    running->loop.post([core = running->core, id = std::string(client_id), group] {
        core->move_client(id, group);
    });
    return true;
}

bool SendspinServer::set_player_format(std::string_view client_id,
                                       std::optional<StreamFormat> format) {
    return this->impl_->set_player_format(client_id, format);
}

bool SendspinServer::set_controller_caps(GroupId group, const ControllerCaps& caps) {
    const std::shared_ptr<Running> running = this->impl_->running_with(group);
    if (!running) {
        return false;
    }
    running->loop.post([core = running->core, group, caps = ControllerCaps(caps)] {
        core->set_controller_caps(group, caps);
    });
    return true;
}

bool SendspinServer::set_group_volume(GroupId group, uint32_t volume) {
    const std::shared_ptr<Running> running = this->impl_->running_with(group);
    if (!running || volume > MAX_VOLUME) {
        return false;
    }
    running->loop.post(
        [core = running->core, group, volume] { core->set_group_volume(group, volume); });
    return true;
}

bool SendspinServer::set_group_mute(GroupId group, bool muted) {
    const std::shared_ptr<Running> running = this->impl_->running_with(group);
    if (!running) {
        return false;
    }
    running->loop.post(
        [core = running->core, group, muted] { core->set_group_mute(group, muted); });
    return true;
}

bool SendspinServer::set_metadata(GroupId group, const Metadata& metadata, int64_t at_us) {
    const auto too_long = [](const std::optional<std::string>& text) {
        return text && text->size() > MAX_JSON_STRING_BYTES;
    };
    const std::shared_ptr<Running> running = this->impl_->running_with(group);
    if (!running || too_long(metadata.title) || too_long(metadata.artist) ||
        too_long(metadata.album_artist) || too_long(metadata.album) ||
        too_long(metadata.artwork_url) ||
        (metadata.progress && metadata.progress->playback_speed > MAX_PLAYBACK_SPEED)) {
        return false;
    }
    running->loop.post([core = running->core, group, metadata = Metadata(metadata), at_us] {
        core->set_metadata(group, metadata, at_us);
    });
    return true;
}

bool SendspinServer::cancel_scheduled_metadata(GroupId group) {
    const std::shared_ptr<Running> running = this->impl_->running_with(group);
    if (!running) {
        return false;
    }
    running->loop.post([core = running->core, group] { core->cancel_scheduled_metadata(group); });
    return true;
}

bool SendspinServer::set_artwork(GroupId group, ArtworkSource source,
                                 std::span<const uint8_t> image, int64_t at_us) {
    const std::shared_ptr<Running> running = this->impl_->running_with(group);
    if (!running || (!image.empty() && !probe_image(image))) {
        return false;
    }
    std::shared_ptr<SourceImage> held;
    if (!image.empty()) {
        held = std::make_shared<SourceImage>();
        held->bytes.assign(image.begin(), image.end());
        // On this thread: hashing a large image would hold core past its handler budget
        Sha256 hash;
        hash.update(held->bytes.data(), held->bytes.size());
        held->hash = hash.finalize();
        if (!hash.ok()) {
            return false;
        }
    }
    running->loop.post([core = running->core, group, source,
                        held = std::shared_ptr<const SourceImage>(std::move(held)),
                        at_us] { core->set_artwork(group, source, held, at_us); });
    return true;
}

bool SendspinServer::cancel_scheduled_artwork(GroupId group, ArtworkSource source) {
    const std::shared_ptr<Running> running = this->impl_->running_with(group);
    if (!running) {
        return false;
    }
    running->loop.post(
        [core = running->core, group, source] { core->cancel_scheduled_artwork(group, source); });
    return true;
}

bool SendspinServer::set_colors(GroupId group, const Colors& colors, int64_t at_us) {
    const std::shared_ptr<Running> running = this->impl_->running_with(group);
    if (!running) {
        return false;
    }
    running->loop.post(
        [core = running->core, group, colors, at_us] { core->set_colors(group, colors, at_us); });
    return true;
}

bool SendspinServer::cancel_scheduled_colors(GroupId group) {
    const std::shared_ptr<Running> running = this->impl_->running_with(group);
    if (!running) {
        return false;
    }
    running->loop.post([core = running->core, group] { core->cancel_scheduled_colors(group); });
    return true;
}

// ==============================================================================
// GroupInput
// ==============================================================================

GroupInput::GroupInput(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

GroupInput::~GroupInput() {
    this->impl_->view->release();
}

InputId GroupInput::id() const {
    return this->impl_->view->id();
}

GroupId GroupInput::group() const {
    return this->impl_->view->group();
}

WriteResult GroupInput::write(const void* pcm, size_t frames) {
    return this->impl_->view->write(pcm, frames);
}

InputState GroupInput::state() const {
    return this->impl_->view->state();
}

InputState GroupInput::wait_writable(int64_t timeout_us) {
    return this->impl_->view->wait_writable(timeout_us);
}

void GroupInput::flush() {
    this->impl_->view->flush();
}

DrainId GroupInput::drain() {
    return this->impl_->view->drain();
}

uint64_t GroupInput::pause() {
    return this->impl_->view->pause();
}

void GroupInput::resume() {
    this->impl_->view->resume();
}

void GroupInput::stop() {
    this->impl_->view->stop();
}

bool GroupInput::set_format(const InputFormat& format) {
    return this->impl_->view->set_format(format);
}

int64_t GroupInput::time_of(uint64_t sample_pos) const {
    return this->impl_->view->time_of(sample_pos);
}

int64_t GroupInput::latency_us() const {
    return this->impl_->view->latency_us();
}

}  // namespace sendspin::server
