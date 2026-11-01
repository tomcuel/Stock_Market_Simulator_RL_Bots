"""
The RL agent of Src_RL_Trading: our own PPO (NumPy) trained by walk-forward on trading days recorded in the simulated exchange, and the policy server the C++ simulator's RLBots ask for their trades

    daily.py       the recorded trading days: daily bars, intraday trades, features, walk-forward folds
    env.py         the environment: one step = one trading day, an entry at the open, a stop-loss and a take-profit
    ppo.py         Proximal Policy Optimization in NumPy
    train.py       walk-forward training, the checkpoint rule, the gate, the ensemble, the sealed markets, the baselines
    policy.py      a trained agent (or a rule baseline) as the server serves it
    serve.py       the policy server (the daily stream per symbol, the protocol)
    protocol.py    the framing shared with the C++ side
    selftest.py    python -m rl_agents.selftest
"""
