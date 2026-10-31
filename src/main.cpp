#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <vector>

using namespace geode::prelude;

struct Spike { float x, y; };
struct Obstacle { float x0, y0, x1, y1; };

class $modify(BotLayer, GJBaseGameLayer) {
	struct Fields {
		// уровень
		std::vector<Spike> hazards;      // шипы (для куба), по X
		std::vector<Obstacle> obstacles; // шипы + блоки (для волны), по X
		std::vector<Obstacle> window;    // препятствия рядом с игроком
		float maxW = 30.f;
		bool built = false;

		// куб
		float lastHandledX = -1.f;
		float releaseTimer = 0.f;
		bool holding = false;

		// волна
		bool waveHold = false;
		bool pending = false;
		float delay = 0.f;
		bool hasPrev = false;
		float prevX = 0.f, prevY = 0.f;
		float slope = 1.f;   // сколько юнитов вверх на 1 юнит вперёд
		float upSign = 1.f;  // +1: зажатие = вверх

		std::mt19937 rng{std::random_device{}()};
	};

	void buildObjects() {
		auto f = m_fields.self();
		f->hazards.clear();
		f->obstacles.clear();
		for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
			auto type = obj->m_objectType;
			if (type == GameObjectType::Hazard) {
				f->hazards.push_back({obj->getPositionX(), obj->getPositionY()});
			}
			if (type == GameObjectType::Hazard || type == GameObjectType::Solid) {
				auto r = obj->getObjectRect();
				f->obstacles.push_back({r.getMinX(), r.getMinY(), r.getMaxX(), r.getMaxY()});
				f->maxW = std::max(f->maxW, r.size.width);
			}
		}
		std::sort(f->hazards.begin(), f->hazards.end(),
			[](Spike const& a, Spike const& b) { return a.x < b.x; });
		std::sort(f->obstacles.begin(), f->obstacles.end(),
			[](Obstacle const& a, Obstacle const& b) { return a.x0 < b.x0; });
		f->built = true;
	}

	// ---------- ВОЛНА: просчёт пути наперёд ----------
	void waveLogic(float dt) {
		auto f = m_fields.self();
		int legit = static_cast<int>(Mod::get()->getSettingValue<int64_t>("legit"));

		float px = m_player1->getPositionX();
		float py = m_player1->getPositionY();

		// учимся: угол подъёма и что значит "зажато"
		if (f->hasPrev) {
			float ddx = px - f->prevX;
			float ddy = py - f->prevY;
			if (ddx > 0.1f && std::abs(ddy) > 0.3f * ddx) {
				f->slope = std::clamp(0.7f * f->slope + 0.3f * std::abs(ddy) / ddx, 0.5f, 3.f);
				f->upSign = ((ddy > 0.f) == f->waveHold) ? 1.f : -1.f;
			}
		}

		float speed = 311.58f * m_player1->m_playerSpeed;
		float H = speed * 0.55f;      // смотрим на ~0.55 сек вперёд
		constexpr int N = 40;
		float dx = H / N;
		float stepY = f->slope * dx * f->upSign;
		float r = std::clamp(5.f * std::abs(m_player1->getScale()), 3.f, 6.f);

		// препятствия в зоне просчёта
		f->window.clear();
		auto it = std::lower_bound(f->obstacles.begin(), f->obstacles.end(),
			px - f->maxW - 10.f,
			[](Obstacle const& o, float v) { return o.x0 < v; });
		for (; it != f->obstacles.end() && it->x0 <= px + H + 20.f; ++it) {
			if (it->x1 >= px - 10.f) f->window.push_back(*it);
		}

		auto isSafe = [&](int i, int u, float rr) {
			float x = px + i * dx;
			float y = py + stepY * float(2 * u - i);
			for (auto const& o : f->window) {
				if (x > o.x0 - rr && x < o.x1 + rr && y > o.y0 - rr && y < o.y1 + rr) return false;
			}
			return true;
		};

		// d[i][u] = сколько шагов подряд можно прожить, если после i шагов
		// мы зажимали кнопку u раз
		auto plan = [&](float rr, int& dHold, int& dRel) {
			std::array<std::array<int, N + 2>, N + 2> d{};
			for (int u = 0; u <= N; ++u) d[N][u] = isSafe(N, u, rr) ? 1 : 0;
			for (int i = N - 1; i >= 1; --i) {
				for (int u = 0; u <= i; ++u) {
					d[i][u] = isSafe(i, u, rr) ? 1 + std::max(d[i + 1][u], d[i + 1][u + 1]) : 0;
				}
			}
			dHold = d[1][1];
			dRel = d[1][0];
		};

		int dh = 0, dr = 0;
		plan(r + 4.f, dh, dr);                 // сначала с запасом от стен
		if (std::max(dh, dr) < N) plan(r, dh, dr); // если не вышло, впритык

		bool want = f->waveHold;
		if (dh > dr) want = true;
		else if (dr > dh) want = false;

		// "человечность": задержка реакции, растёт с legit
		if (want != f->waveHold) {
			if (!f->pending) {
				f->pending = true;
				if (legit > 0) {
					std::uniform_real_distribution<float> d(0.f, legit * 0.012f);
					f->delay = d(f->rng);
				} else {
					f->delay = 0.f;
				}
			}
			f->delay -= dt;
			if (f->delay > 0.f) want = f->waveHold;
		} else {
			f->pending = false;
		}

		if (want != f->waveHold) {
			this->handleButton(want, 1, true);
			f->waveHold = want;
			f->pending = false;
		}

		f->prevX = px;
		f->prevY = py;
		f->hasPrev = true;
	}

	// ---------- КУБ: прыжок перед шипом ----------
	void cubeLogic(float dt) {
		auto f = m_fields.self();

		if (f->holding) {
			f->releaseTimer -= dt;
			if (f->releaseTimer <= 0.f) {
				this->handleButton(false, 1, true);
				f->holding = false;
			}
			return;
		}

		float px = m_player1->getPositionX();
		float py = m_player1->getPositionY();
		if (px < f->lastHandledX) f->lastHandledX = -1.f;
		if (!m_player1->m_isOnGround) return;

		int legit = static_cast<int>(Mod::get()->getSettingValue<int64_t>("legit"));
		float speed = 311.58f * m_player1->m_playerSpeed;
		float baseLead = speed * 0.17f;

		auto it = std::lower_bound(f->hazards.begin(), f->hazards.end(), px,
			[](Spike const& h, float x) { return h.x < x; });

		for (; it != f->hazards.end(); ++it) {
			float ddx = it->x - px;
			if (ddx > baseLead + 60.f) break;
			if (std::abs(it->y - py) > 40.f) continue;
			if (it->x <= f->lastHandledX) continue;

			std::normal_distribution<float> noise(0.f, legit * 1.6f);
			float lead = baseLead + noise(f->rng);

			if (ddx <= lead) {
				f->lastHandledX = it->x;
				std::uniform_real_distribution<float> chance(0.f, 1.f);
				bool miss = chance(f->rng) < legit * 0.035f;
				if (miss) break;
				this->handleButton(true, 1, true);
				f->holding = true;
				f->releaseTimer = 0.06f;
				break;
			}
			break;
		}
	}

	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

		if (static_cast<GJBaseGameLayer*>(PlayLayer::get()) != this) return;
		if (!Mod::get()->getSettingValue<bool>("enabled")) return;
		if (!m_player1 || m_player1->m_isDead) return;

		auto f = m_fields.self();
		if (!f->built) this->buildObjects();

		if (m_player1->m_isDart) {
			if (f->holding) {
				this->handleButton(false, 1, true);
				f->holding = false;
			}
			this->waveLogic(dt);
			return;
		}

		// вышли из волны: отпускаем кнопку
		if (f->waveHold) {
			this->handleButton(false, 1, true);
			f->waveHold = false;
		}
		f->hasPrev = false;
		f->pending = false;

		bool cubeLike = !(m_player1->m_isShip || m_player1->m_isBird || m_player1->m_isBall ||
			m_player1->m_isRobot || m_player1->m_isSpider || m_player1->m_isSwing);
		if (cubeLike) this->cubeLogic(dt);
	}
};
