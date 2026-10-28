# Trader

A small Linux-first C++20 scaffold for an Indian-market trading system. It separates the polling loop, strategy, broker adapter, and browser control plane so a small team can extend each piece independently.

## Current scope

- A one-second market-data and order-update polling loop with separate Analyzer and Trader modules.
- A versioned `AnalysisResult` handoff carrying signed target positions, strategy ID, sequence number, freshness deadline, rationale, and optional option-contract metadata.
- An Analyzer that loads historical bars through `MarketDataProvider`, initializes business logic, and scopes/validates candidates.
- A Trader that converts desired positions into order deltas after accounting for current positions and pending orders, then independently rechecks freshness, strategy identity, lot size, capital, margin, and risk limits.
- A `Trader` lifecycle and state module with configured-symbol, lot-size, capital, position-size, stop-loss, and drawdown checks.
- Active-order tracking, cumulative partial-fill accounting, realized/unrealized PnL, cancel/modify hooks, and square-off support.
- F&O contract metadata, Black-Scholes Greeks/implied-volatility calculation, strike selection, expiry cutoff and rollover hooks, broker-provided margin checks, multi-leg submission, and delta/gamma hedge hooks.
- A broker interface with a paper implementation selected by default and a Zerodha Kite Connect REST adapter.
- A local WebSocket server on `127.0.0.1:8765` and a minimal browser page in `web/index.html`.
- Timestamped logging using the standard library.

This is not a production trading application. The paper broker supplies no market data. The Kite Connect adapter supports REST quote polling, historical candles, regular order placement/cancel/modify, order updates, and order margin queries. Kite credentials are never stored in the repository. The adapter expects a daily access token obtained through Zerodha's login flow; token generation/refresh and Kite's streaming ticker WebSocket are not implemented. Live orders remain disabled unless explicitly enabled, and the configured risk limits must be reviewed before use.

The scheduler is the central process: it owns the broker and Trader, routes market data and timer events to separate Analyzer worker processes, and sends their versioned `AnalysisResult` blocks through Trader's existing validation and risk checks. A fixed Analyzer thread pool is started before connections are accepted. The scheduler's main thread listens and accepts worker connections, then dispatches each persistent worker connection to one of those prestarted threads. A separate polling thread retrieves broker updates and market data. Analyzer workers connect over loopback TCP and receive their assigned symbol subset and historical bars during initialization. The scheduler assigns a single monotonic sequence to results from all workers. The browser-facing WebSocket remains a separate local interface.

Inter-process messages use UTF-8 text blocks with a `BEGIN <type>` line, escaped `key=value` fields, and an `END` line. The protocol currently supports one scheduler with a pool of Analyzer processes partitioned by configured symbols. Each Analyzer process runs the base `BusinessLogic`, which is deliberately a skeleton; add the desired strategy logic in the worker before using this as a trading system. The scheduler-worker channel binds to loopback and has no authentication; do not expose it to untrusted networks.

Risk limits set to zero are disabled. Configure meaningful limits before enabling any real broker. `max_drawdown` and `stop_loss_limit` are currency amounts; `max_position_size` is absolute quantity per exchange-qualified symbol.

The executable reads `TRADER_BROKER`, `TRADER_STRATEGY_ID`, comma-separated `TRADER_SYMBOLS`, `TRADER_ALLOCATED_CAPITAL`, `TRADER_MAX_DRAWDOWN`, `TRADER_STOP_LOSS_LIMIT`, `TRADER_MAX_POSITION_SIZE`, and `TRADER_LIVE_ORDERS`. Defaults are the paper broker, a paper strategy over `RELIANCE,TCS`, INR 100,000 allocated, and disabled risk limits. `TRADER_SYMBOLS` can use `EXCHANGE:SYMBOL` entries (for example `NSE:RELIANCE,NFO:NIFTY...`); unqualified symbols use `TRADER_KITE_DEFAULT_EXCHANGE`, which defaults to `NSE`.

## Build

Dependencies: a C++20 compiler, CMake 3.20+, Boost.System/Boost.Beast headers, and OpenSSL.

On Debian or Ubuntu:

```sh
sudo apt install build-essential cmake libboost-dev libssl-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Start the scheduler, then launch one or more Analyzer workers in separate terminals (worker count must not exceed configured symbols):

```sh
./build/trader scheduler 8766 2
./build/trader analyzer
./build/trader analyzer
```

The default `scheduler` port is `8766`; an Analyzer connects to `127.0.0.1:8766`. The scheduler's browser WebSocket is still on `127.0.0.1:8765`. Open `web/index.html` in a browser and connect to the local backend. Neither local endpoint has authentication; do not expose them to an untrusted network.

To select Zerodha, export credentials and set the adapter configuration before starting the scheduler:

```sh
export TRADER_BROKER=zerodha
export TRADER_KITE_API_KEY=your_api_key
export TRADER_KITE_ACCESS_TOKEN=your_daily_access_token
export TRADER_KITE_DEFAULT_EXCHANGE=NSE
export TRADER_KITE_PRODUCT=CNC
export TRADER_KITE_HISTORY_INTERVAL=day
export TRADER_KITE_HISTORY_DAYS=30
export TRADER_SYMBOLS=NSE:RELIANCE,NSE:TCS
# Leave false until credentials, risk limits, and orders have been validated.
export TRADER_LIVE_ORDERS=false
./build/trader scheduler 8766 2
```

The credentials must be provisioned out-of-band from the Kite Connect login flow; the API secret is not required by the adapter and must not be put in the application environment. Choose `CNC`, `MIS`, or `NRML` to match the instruments and trading plan (for example, derivatives generally require `NRML` or `MIS`). First validate the integration with the paper broker and review instrument symbols, market-hours behavior, risk limits, and order semantics before enabling live orders. Quotes are polled over REST once per scheduler cycle, not streamed.

The adapter follows Zerodha's [Kite Connect v3 authentication](https://kite.trade/docs/connect/v3/user/), [market-data and instruments](https://kite.trade/docs/connect/v3/market-data-and-instruments/), [historical candles](https://kite.trade/docs/connect/v3/historical/), [orders](https://kite.trade/docs/connect/v3/orders/), and [margin calculation](https://kite.trade/docs/connect/v3/margins/) endpoints.

## WebSocket methods

Messages are UTF-8 text commands; server responses are pipe-delimited text.

| Request | Response / behavior |
| --- | --- |
| `get_status` | `status|ok|broker=paper|live_orders=disabled` |
| `subscribe_market_data` | `subscribed|market_data`; subsequent ticks are `tick|exchange|symbol|last_price|timestamp_ms` |
| `unsubscribe_market_data` | `unsubscribed|market_data` |
| `place_order` | `error|live_orders_disabled` |

The protocol is deliberately small and versionable. Validate every command and add authentication before exposing it beyond localhost.

## Extension points

- `KiteBroker` implements Kite Connect REST operations; implement `Broker` for other providers in their own adapter. Keep broker credentials and token handling out of the strategy and frontend.
- Map broker instruments to explicit exchange-qualified symbols, such as `NSE|...` and `BSE|...`; NSE/BSE access depends on the chosen broker account and instrument availability.
- Derive from `BusinessLogic` and implement `evaluate_signals`, `on_bar`, and `on_timer`; these callbacks return desired signed target positions, not broker orders. Analyzer wraps candidates in `AnalysisResult`; Trader converts target positions into orders.
- Candidate sequences must increase per strategy. Results use schema version 1 and default to a three-second freshness window. `generated_at_ms` and `expires_at_ms` are Unix epoch milliseconds; the Trader rejects stale, duplicate, mismatched-strategy, malformed, or expired inputs.
- Configure the strategy and complete symbol universe in the scheduler; it assigns disjoint symbol subsets to workers. Keep Analyzer output declarative: the Trader owns broker state, order timing, and final risk decisions.
- Broker adapters should implement historical data, cumulative order updates, cancel/modify, and `get_margin`. F&O order placement fails closed when the broker does not return valid margin data.
- Greeks use European Black-Scholes without dividends; the default risk-free rate is 6%, theta is per calendar day, and vega is per one volatility percentage point. Validate market conventions and model assumptions for the specific contract.
- Expiry checks receive the exchange-local date and minutes after midnight; the default cutoff is 15:15 local time and must be set according to the broker/exchange rules. No background scheduler invokes the cutoff automatically yet.
- Multi-leg orders are sequential, not atomic. A failed later leg triggers best-effort cancellation of earlier working orders; already-filled legs require separate recovery.
- `stop()` cancels working orders but intentionally leaves positions open. Call `square_off_all()` explicitly when the operational policy requires liquidation.
- Run `ctest --test-dir build --output-on-failure` after building. Persistence, reconnect/backoff, and broker certification are still needed before unattended operation.
