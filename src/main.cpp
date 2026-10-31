#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <vector>

using namespace geode::prelude;

struct Obstacle { float x0, y0, x1, y1; bool hazard; };

class $modify(BotLayer, GJBaseGameLayer) {
	struct Fields {
		std::vector<Obstacle> obstacles; // шипы + блоки, отсортированы по x0
		std::vector<Obstacle> window;    // препятствия рядом с игроком (для волны)
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
		float slope = 1.f;   // юнитов вверх на 1 юнит вперёд
		float upSign = 1.f;  // +1: зажатие = вверх, -1: зажатие = вниз

		std::mt19937 rng{std::random_device{}()};
	};

	void buildObjects() {
		auto f = m_fields.self();
		f->obstacles.clear();
		for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
			auto type = obj->m_objectType;
			if (type == GameObjectType::Hazard || type == GameObjectType::Solid) {
				auto r = obj->getObjectRect();
				f->obstacles.push_back({
					r.getMinX(), r.getMinY(), r.getMaxX(), r.getMaxY(),
					type == GameObjectType::Hazard
				});
				f->maxW = std::max(f->maxW, r.size.width);
			}
		}
		std::sort(f->obstacles.begin(), f->obstacles.end(),
			[](Obstacle const& a, Obstacle const& b) { return a.x0 < b.x0; });
		f->built = true;
	}

	// ================= ВОЛНА =================
	// Перебираем все варианты "вверх/вниз" на ~0.5 сек вперёд.
	// Шипы убивают при касании. Блоки убивают только боком,
	// а сверху/снизу волна по ним скользит (как в игре).
	void waveLogic(float dt) {
		auto f = m_fields.self();
		int legit = static_cast<int>(Mod::get()->getSettingValue<int64_t>("legit"));

		float px = m_player1->getPositionX();
		float py = m_player1->getPositionY();

		// учимся: угол волны и что значит "зажато"
		if (f->hasPrev) {
			float ddx = px - f->prevX;
			float ddy = py - f->prevY;
			if (ddx > 0.1f && std::abs(ddy) > 0.3f * ddx) {
				f->slope = std::clamp(0.7f * f->slope + 0.3f * std::abs(ddy) / ddx, 0.5f, 3.f);
				f->upSign = ((ddy > 0.f) == f->waveHold) ? 1.f : -1.f;
			}
		}

		constexpr int N = 32;
		constexpr int DEAD = -1000;
		constexpr int OFF = N + 1;
		constexpr int W = 2 * N + 3;

		float speed = 311.58f * m_player1->m_playerSpeed;
		float H = speed * 0.55f;
		float dx = H / N;
		float step = f->slope * dx;
		float r = std::clamp(5.f * std::abs(m_player1->getScale()), 3.f, 6.f);
		float yRange = N * step + 40.f;

		// препятствия вокруг (по X и по Y)
		f->window.clear();
		auto it = std::lower_bound(f->obstacles.begin(), f->obstacles.end(),
			px - f->maxW - 10.f,
			[](Obstacle const& o, float v) { return o.x0 < v; });
		for (; it != f->obstacles.end() && it->x0 <= px + H + 20.f; ++it) {
			if (it->x1 < px - 10.f) continue;
			if (it->y1 < py - yRange || it->y0 > py + yRange) continue;
			f->window.push_back(*it);
		}

		auto blocked = [&](float x, float y, float rr, bool& hazard) {
			hazard = false;
			bool any = false;
			for (auto const& o : f->window) {
				if (x > o.x0 - rr && x < o.x1 + rr && y > o.y0 - rr && y < o.y1 + rr) {
					if (o.hazard) { hazard = true; return true; }
					any = true;
				}
			}
			return any;
		};

		// шаг i -> i+1 по решётке высот: y = py + k * step
		auto move = [&](int i, int k, bool up, float rr) {
			float x = px + (i + 1) * dx;
			int nk = k + (up ? 1 : -1);
			bool hz = false;
			if (!blocked(x, py + nk * step, rr, hz)) return nk;
			if (hz) return DEAD;                               // шип
			if (!blocked(x, py + k * step, rr, hz)) return k;   // скольжение по блоку
			return DEAD;                                       // врезались боком
		};

		// d[i][k] = сколько шагов ещё можно прожить из состояния (i, k)
		auto plan = [&](float rr, int& dUp, int& dDown) {
			std::array<std::array<int, W>, N + 1> d{};
			for (int i = N - 1; i >= 1; --i) {
				for (int k = -i; k <= i; ++k) {
					int best = 0;
					for (int dir = 0; dir < 2; ++dir) {
						int nk = move(i, k, dir == 1, rr);
						if (nk != DEAD) best = std::max(best, 1 + d[i + 1][nk + OFF]);
					}
					d[i][k + OFF] = best;
				}
			}
			int nu = move(0, 0, true, rr);
			int nd = move(0, 0, false, rr);
			dUp = (nu != DEAD) ? 1 + d[1][nu + OFF] : 0;
			dDown = (nd != DEAD) ? 1 + d[1][nd + OFF] : 0;
		};

		// сначала стараемся держаться подальше от стен, потом впритык
		int dUp = 0, dDown = 0;
		float tiers[3] = {r + 5.f, r + 2.5f, r};
		for (float rr : tiers) {
			plan(rr, dUp, dDown);
			if (std::max(dUp, dDown) >= N) break;
		}

		bool holdMeansUp = f->upSign > 0.f;
		bool curUp = (f->waveHold == holdMeansUp);
		bool goUp = curUp;
		if (dUp > dDown) goUp = true;
		else if (dDown > dUp) goUp = false;
		bool want = (goUp == holdMeansUp);

		// человечность: задержка реакции растёт с legit (0 = мгновенно)
		if (want != f->waveHold) {
			if (!f->pending) {
				f->pending = true;
				if (legit > 0) {
					std::uniform_real_distribution<float> dist(0.f, legit * 0.012f);
					f->delay = dist(f->rng);
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

	// ================= КУБ =================
	// Прыгаем, только если препятствие реально на уровне игрока
	// (шип на блок выше или ниже игнорируется).
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

		auto it = std::lower_bound(f->obstacles.begin(), f->obstacles.end(), px,
			[](Obstacle const& o, float x) { return o.x0 < x; });

		for (; it != f->obstacles.end(); ++it) {
			float ddx = it->x0 - px;
			if (ddx > baseLead + 60.f) break;

			// пересекается ли препятствие по высоте с телом куба?
			if (!(it->y1 > py - 10.f && it->y0 < py + 10.f)) continue;
			if (it->x0 <= f->lastHandledX) continue;

			std::normal_distribution<float> noise(0.f, legit * 1.6f);
			float lead = baseLead + noise(f->rng);

			if (ddx <= lead) {
				f->lastHandledX = it->x0;
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
