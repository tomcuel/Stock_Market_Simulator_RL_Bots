"""
Proximal Policy Optimization: the RL agent's learner
- two networks, a policy and a value function, each an MLP with tanh hidden layers (default 128, 64)
- a factorized discrete policy: one softmax head per action component (direction, stop-loss bucket, take-profit bucket), the action's log-probability is the sum of the heads'
- rollouts of n_steps transitions, advantages by GAE(lambda), normalized per minibatch
- the clipped surrogate objective, an entropy bonus, a squared-error value loss
- n_epochs passes per rollout, early stop when the approximate KL divergence passes 1.5 x target_kl
- Adam with weight decay, the learning rate decaying linearly to 0, gradients clipped to a norm of 0.5
- observations normalized by running mean and variance (what VecNormalize does), saved with the model: a checkpoint is always used with the normalization it was trained and selected with; rewards scaled by the running std of the return

Everything is plain arrays, so a saved model (PPOAgent.state()) is a dict of arrays and numbers that pickles anywhere
"""
from __future__ import annotations

import copy
from dataclasses import dataclass, field

import numpy as np


# ---------------------------------------------------------------- building blocks
class RunningNorm:
    """
    Running mean and variance (parallel algorithm), for observations or returns
    """
    def __init__(self, shape=(), clip: float = 10.0, epsilon: float = 1e-8):
        self.mean = np.zeros(shape)
        self.var = np.ones(shape)
        self.count = epsilon
        self.clip = clip

    def update(self, x: np.ndarray) -> None:
        x = np.asarray(x, dtype=float)
        if x.ndim == len(self.mean.shape):
            x = x[None]
        batch_mean, batch_var, n = x.mean(axis=0), x.var(axis=0), x.shape[0]
        delta = batch_mean - self.mean
        total = self.count + n
        self.mean = self.mean + delta * n / total
        self.var = (self.var * self.count + batch_var * n + delta ** 2 * self.count * n / total) / total
        self.count = total

    def normalize(self, x: np.ndarray) -> np.ndarray:
        return np.clip((np.asarray(x, dtype=float) - self.mean) / np.sqrt(self.var + 1e-8), -self.clip, self.clip)


class MLP:
    """
    tanh hidden layers, a linear output: forward() keeps what backward() needs
    """
    def __init__(self, sizes: list[int], rng: np.random.Generator, out_gain: float = 1.0):
        self.W, self.b = [], []
        for i, (n_in, n_out) in enumerate(zip(sizes[:-1], sizes[1:])):
            gain = out_gain if i == len(sizes) - 2 else np.sqrt(2.0)
            # orthogonal initialization, as SB3 (gain sqrt(2) for hidden layers, small for the outputs)
            a = rng.standard_normal((max(n_in, n_out), min(n_in, n_out)))
            q, r = np.linalg.qr(a)
            q = q * np.sign(np.diag(r))
            w = q if n_in >= n_out else q.T
            self.W.append(gain * w[:n_in, :n_out])
            self.b.append(np.zeros(n_out))

    def forward(self, x: np.ndarray):
        activations = [x]
        h = x
        for i, (w, b) in enumerate(zip(self.W, self.b)):
            z = h @ w + b
            h = np.tanh(z) if i < len(self.W) - 1 else z
            activations.append(h)
        return h, activations

    def backward(self, activations, grad_out: np.ndarray):
        grads_w, grads_b = [None] * len(self.W), [None] * len(self.W)
        g = grad_out
        for i in reversed(range(len(self.W))):
            grads_w[i] = activations[i].T @ g
            grads_b[i] = g.sum(axis=0)
            if i > 0:
                g = (g @ self.W[i].T) * (1.0 - activations[i] ** 2) # through the tanh of layer i-1
        return grads_w, grads_b

    def params(self) -> list[np.ndarray]:
        return self.W + self.b


class Adam:
    def __init__(self, params: list[np.ndarray], weight_decay: float = 0.0):
        self.m = [np.zeros_like(p) for p in params]
        self.v = [np.zeros_like(p) for p in params]
        self.t = 0
        self.weight_decay = weight_decay

    def step(self, params: list[np.ndarray], grads: list[np.ndarray], lr: float, max_norm: float = 0.5) -> None:
        norm = np.sqrt(sum(float((g ** 2).sum()) for g in grads))
        scale = min(1.0, max_norm / (norm + 1e-12))
        self.t += 1
        for p, g, m, v in zip(params, grads, self.m, self.v):
            g = g * scale + self.weight_decay * p
            m *= 0.9
            m += 0.1 * g
            v *= 0.999
            v += 0.001 * g * g
            m_hat = m / (1 - 0.9 ** self.t)
            v_hat = v / (1 - 0.999 ** self.t)
            p -= lr * m_hat / (np.sqrt(v_hat) + 1e-8)


def log_softmax(z: np.ndarray) -> np.ndarray:
    z = z - z.max(axis=-1, keepdims=True)
    return z - np.log(np.exp(z).sum(axis=-1, keepdims=True))


# ---------------------------------------------------------------- the agent
@dataclass
class PPOConfig:
    hidden: tuple = (128, 64)
    learning_rate: float = 3e-4
    lr_schedule: str = "linear"   # linear: decays to 0 over training, constant
    n_steps: int = 2048
    batch_size: int = 256
    n_epochs: int = 5
    gamma: float = 0.99
    gae_lambda: float = 0.95
    clip_range: float = 0.1
    ent_coef: float = 0.03
    vf_coef: float = 0.5
    target_kl: float | None = 0.025
    weight_decay: float = 1e-5
    max_grad_norm: float = 0.5
    normalize_reward: bool = True
    seed: int = 0
    extra: dict = field(default_factory=dict)


class PPOAgent:
    def __init__(self, obs_dim: int, heads: list[int], config: PPOConfig):
        self.obs_dim, self.heads, self.config = obs_dim, list(heads), config
        self.rng = np.random.default_rng(config.seed)
        hidden = list(config.hidden)
        self.pi = MLP([obs_dim] + hidden + [sum(heads)], self.rng, out_gain=0.01)
        self.vf = MLP([obs_dim] + hidden + [1], self.rng, out_gain=1.0)
        self.pi_opt = Adam(self.pi.params(), config.weight_decay)
        self.vf_opt = Adam(self.vf.params(), config.weight_decay)
        self.obs_norm = RunningNorm((obs_dim,))
        self.ret_norm = RunningNorm(())
        self.timesteps = 0

    # ---- acting
    def _split(self, logits: np.ndarray) -> list[np.ndarray]:
        out, start = [], 0
        for n in self.heads:
            out.append(logits[:, start:start + n])
            start += n
        return out

    def distribution(self, obs_norm: np.ndarray) -> list[np.ndarray]:
        """
        Per head, the log-probabilities of every choice, for normalized observations (batch x obs_dim)
        """
        logits, _ = self.pi.forward(obs_norm)
        return [log_softmax(z) for z in self._split(logits)]

    def act(self, obs: np.ndarray, deterministic: bool = False, update_norm: bool = False):
        """
        obs: one raw observation (obs_dim,) -> (action list, log-prob, value)
        """
        obs = np.asarray(obs, dtype=float)
        if update_norm:
            self.obs_norm.update(obs)
        x = self.obs_norm.normalize(obs)[None]
        logps = self.distribution(x)
        action = []
        logp = 0.0
        for lp in logps:
            p = np.exp(lp[0])
            a = int(np.argmax(p)) if deterministic else int(self.rng.choice(len(p), p=p / p.sum()))
            action.append(a)
            logp += float(lp[0, a])
        value = float(self.vf.forward(x)[0][0, 0])
        return action, logp, value

    def probabilities(self, obs: np.ndarray) -> list[np.ndarray]:
        x = self.obs_norm.normalize(np.asarray(obs, dtype=float))[None]
        return [np.exp(lp[0]) for lp in self.distribution(x)]

    # ---- learning
    def learning_rate(self, progress: float) -> float:
        """
        progress: share of the training budget still to come (1 at the start, 0 at the end)
        """
        lr = self.config.learning_rate
        return lr * max(progress, 0.0) if self.config.lr_schedule == "linear" else lr

    def policy_gradients(self, obs_n, actions, old_logp, advantages):
        """
        Loss and gradients of the clipped surrogate + entropy bonus for one minibatch (also used by the gradient test)
        """
        c = self.config
        logits, acts = self.pi.forward(obs_n)
        heads = self._split(logits)
        batch = len(obs_n)
        logp = np.zeros(batch)
        entropy = np.zeros(batch)
        parts = []
        for k, z in enumerate(heads):
            lp = log_softmax(z)
            p = np.exp(lp)
            logp += lp[np.arange(batch), actions[:, k]]
            h = -(p * lp).sum(axis=1)
            entropy += h
            parts.append((lp, p, h))
        ratio = np.exp(logp - old_logp)
        s1 = ratio * advantages
        s2 = np.clip(ratio, 1 - c.clip_range, 1 + c.clip_range) * advantages
        loss = -np.minimum(s1, s2).mean() - c.ent_coef * entropy.mean()
        d_logp = -(ratio * advantages) * (s1 <= s2) / batch
        grad_logits = []
        for k, (lp, p, h) in enumerate(parts):
            onehot = np.zeros_like(p)
            onehot[np.arange(batch), actions[:, k]] = 1.0
            d_entropy = -p * (lp + h[:, None]) # dH/dz
            grad_logits.append(d_logp[:, None] * (onehot - p) - c.ent_coef * d_entropy / batch)
        gw, gb = self.pi.backward(acts, np.concatenate(grad_logits, axis=1))
        approx_kl = float(np.mean((ratio - 1) - (logp - old_logp)))
        clip_fraction = float(np.mean(np.abs(ratio - 1) > c.clip_range))
        return loss, gw + gb, approx_kl, clip_fraction, float(entropy.mean())

    def value_gradients(self, obs_n, returns):
        value, acts = self.vf.forward(obs_n)
        err = value[:, 0] - returns
        loss = 0.5 * self.config.vf_coef * float(np.mean(err ** 2))
        gw, gb = self.vf.backward(acts, (self.config.vf_coef * err / len(obs_n))[:, None])
        return loss, gw + gb

    def update(self, rollout: dict, progress: float) -> dict:
        """
        One PPO update on a rollout: obs (raw), actions, logp, rewards (raw), values, dones, last_value
        """
        c = self.config
        obs_n = self.obs_norm.normalize(rollout["obs"])
        rewards = rollout["rewards_scaled"]
        values, dones = rollout["values"], rollout["dones"]
        n = len(rewards)
        advantages = np.zeros(n)
        last_gae = 0.0
        for t in reversed(range(n)):
            next_value = rollout["last_value"] if t == n - 1 else values[t + 1]
            next_nonterminal = 1.0 - dones[t]
            delta = rewards[t] + c.gamma * next_value * next_nonterminal - values[t]
            last_gae = delta + c.gamma * c.gae_lambda * next_nonterminal * last_gae
            advantages[t] = last_gae
        returns = advantages + values
        lr = self.learning_rate(progress)
        stats = {"approx_kl": 0.0, "clip_fraction": 0.0, "entropy": 0.0, "policy_loss": 0.0, "value_loss": 0.0, "epochs": 0}
        stopped = False
        for epoch in range(c.n_epochs):
            order = self.rng.permutation(n)
            kls = []
            for start in range(0, n, c.batch_size):
                idx = order[start:start + c.batch_size]
                adv = advantages[idx]
                adv = (adv - adv.mean()) / (adv.std() + 1e-8)
                loss, grads, kl, clip, ent = self.policy_gradients(obs_n[idx], rollout["actions"][idx], rollout["logp"][idx], adv)
                kls.append(kl)
                if c.target_kl is not None and kl > 1.5 * c.target_kl:
                    stopped = True
                    break
                self.pi_opt.step(self.pi.params(), grads, lr, c.max_grad_norm)
                v_loss, v_grads = self.value_gradients(obs_n[idx], returns[idx])
                self.vf_opt.step(self.vf.params(), v_grads, lr, c.max_grad_norm)
                stats.update(policy_loss=loss, value_loss=v_loss, clip_fraction=clip, entropy=ent)
            stats["approx_kl"] = float(np.mean(kls)) if kls else 0.0
            stats["epochs"] = epoch + 1
            if stopped:
                break
        stats["explained_variance"] = float(1 - np.var(returns - values) / (np.var(returns) + 1e-12))
        return stats

    def collect(self, env_factory, n_steps: int, state: dict) -> dict:
        """
        Runs the current (stochastic) policy for n_steps transitions. state carries the env and its running return
        """
        obs_list, act_list, logp_list, rew_list, rew_s_list, val_list, done_list = [], [], [], [], [], [], []
        env, obs, running = state.get("env"), state.get("obs"), state.get("running_return", 0.0)
        if env is None:
            env = env_factory()
            obs = env.reset()
        for _ in range(n_steps):
            self.obs_norm.update(obs)
            action, logp, value = self.act(obs)
            next_obs, reward, done, info = env.step(action)
            running = running * self.config.gamma + reward
            if self.config.normalize_reward:
                self.ret_norm.update(np.array([running]))
                scaled = float(np.clip(reward / np.sqrt(self.ret_norm.var + 1e-8), -10, 10))
            else:
                scaled = reward
            obs_list.append(obs)
            act_list.append(action)
            logp_list.append(logp)
            rew_list.append(reward)
            rew_s_list.append(scaled)
            val_list.append(value)
            done_list.append(float(done))
            if done:
                env = env_factory()
                obs = env.reset()
                running = 0.0
            else:
                obs = next_obs
        self.timesteps += n_steps
        last_value = float(self.vf.forward(self.obs_norm.normalize(obs)[None])[0][0, 0])
        state.update(env=env, obs=obs, running_return=running)
        return {"obs": np.array(obs_list), "actions": np.array(act_list, dtype=int), "logp": np.array(logp_list),
                "rewards": np.array(rew_list), "rewards_scaled": np.array(rew_s_list), "values": np.array(val_list),
                "dones": np.array(done_list), "last_value": last_value}

    # ---- saving
    def state(self) -> dict:
        return {"obs_dim": self.obs_dim, "heads": self.heads, "config": self.config.__dict__.copy(),
                "pi": [w.copy() for w in self.pi.params()], "vf": [w.copy() for w in self.vf.params()],
                "obs_norm": (self.obs_norm.mean.copy(), self.obs_norm.var.copy(), self.obs_norm.count),
                "ret_norm": (self.ret_norm.mean.copy(), self.ret_norm.var.copy(), self.ret_norm.count), "timesteps": self.timesteps}

    @classmethod
    def from_state(cls, state: dict) -> "PPOAgent":
        config = PPOConfig(**{k: (tuple(v) if k == "hidden" else v) for k, v in state["config"].items()})
        agent = cls(state["obs_dim"], state["heads"], config)
        layers = len(agent.pi.W)
        agent.pi.W, agent.pi.b = [w.copy() for w in state["pi"][:layers]], [b.copy() for b in state["pi"][layers:]]
        agent.vf.W, agent.vf.b = [w.copy() for w in state["vf"][:layers]], [b.copy() for b in state["vf"][layers:]]
        agent.pi_opt = Adam(agent.pi.params(), config.weight_decay)
        agent.vf_opt = Adam(agent.vf.params(), config.weight_decay)
        agent.obs_norm.mean, agent.obs_norm.var, agent.obs_norm.count = state["obs_norm"]
        agent.ret_norm.mean, agent.ret_norm.var, agent.ret_norm.count = state["ret_norm"]
        agent.timesteps = state.get("timesteps", 0)
        return agent

    def snapshot(self) -> "PPOAgent":
        return PPOAgent.from_state(copy.deepcopy(self.state()))
