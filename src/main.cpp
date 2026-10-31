#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <vector>

using namespace geode::prelude;

enum { K_HAZARD = 0, K_BLOCK = 1, K_SLOPE = 2, K_HSLOPE = 3 };

struct Obstacle {
	float x0, y0, x1, y1;   // габарит
	int kind;
	// для наклонов: точка A на гипотенузе и нормаль (в сторону твёрдой части)
	float ax, ay, nx, ny;
};

static float speedUnits(float m) {
	if (m < 0.8f) return 251.16f;
	if (m < 1.0f) return 311.58f;
	if (m < 1.2f) return 387.42f;
	if (m < 1.45f) return 468.0f;
	return 576.0f;
}

// пересекается ли квадрат (центр x,y; полуразмер rr) с препятствием
static bool overlapsObs(Obstacle const& o, float x, float y, float rr) {
	if (!(x > o.x0 - rr && x < o.x1 + rr && y > o.y0 - rr && y < o.y1 + rr)) return false;
	if (o.kind == K_SLOPE || o.kind == K_HSLOPE) {
		float fc = o.nx * (x - o.ax) + o.ny * (y - o.ay);
		return fc + rr * (std::abs(o.nx) + std::abs(o.ny)) > 0.f;
	}
	return true;
}

class $modify(BotLayer, GJBaseGameLayer) {
	struct Fields {
		std::vector<Obstacle> obstacles; // отсортированы по x0
		std::vector<Obstacle> window;
		float maxW = 30.f;
		bool built = false;

		// куб
		bool holding = false;
		float releaseTimer = 0.f;
		float w0 = -1.f;
		float jitter = 0.f;
		bool willMiss = false;

		// волна
		bool waveHold = false;
		bool pending = false;
		float delay = 0.f;
		bool hasPrev = false;
		float prevX = 0.f, prevY = 0.f;
		float slope = 1.f;
		float upSign = 1.f;

		std::mt19937 rng{std::random_device{}()};
	};

	void buildObjects() {
		auto f = m_fields.self();
		f->obstacles.clear();
		for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
			auto type = obj->m_objectType;
			bool isSlope = type == GameObjectType::Slope;
			bool isSolid = type == GameObjectType::Solid;
			bool isHazard = type == GameObjectType::Hazard;
			if (!isSlope && !isSolid && !isHazard) continue;
			if (obj->m_isNoTouch) continue;

			auto r = obj->getObjectRect();
			Obstacle o{r.getMinX(), r.getMinY(), r.getMaxX(), r.getMaxY(), K_BLOCK, 0.f, 0.f, 0.f, 0.f};
			if (isHazard) o.kind = K_HAZARD;

			if (isSlope) {
				o.kind = obj->m_slopeIsHazard ? K_HSLOPE : K_SLOPE;

				// базовый наклон: прямой угол справа внизу, потом отражения и поворот
				float sx = 1.f, sy = -1.f;
				if (obj->isFlipX()) sx = -sx;
				if (obj->isFlipY()) sy = -sy;
				int q = ((static_cast<int>(std::lround(obj->getRotation() / 90.f)) % 4) + 4) % 4;
				for (int j = 0; j < q; ++j) {
					float t = sx;
					sx = sy;
					sy = -t;
				}
				float cx = sx > 0.f ? o.x1 : o.x0;
				float cy = sy > 0.f ? o.y1 : o.y0;

				// сверяем со штатной линией наклона из игры
				float ya = static_cast<float>(obj->slopeYPos(o.x0));
				float yb = static_cast<float>(obj->slopeYPos(o.x1));
				bool okLine = ya > o.y0 - 2.f && ya < o.y1 + 2.f && yb > o.y0 - 2.f && yb < o.y1 + 2.f;
				if (okLine) {
					float yAtC = (cx == o.x0) ? ya : yb;
					if (std::abs(cy - yAtC) < 2.f) cx = (cx == o.x0) ? o.x1 : o.x0;
				}

				float ox = (cx == o.x0) ? o.x1 : o.x0;
				float oy = (cy == o.y0) ? o.y1 : o.y0;
				// гипотенуза: A=(ox, cy) -> B=(cx, oy)
				float dx = cx - ox, dy = oy - cy;
				float len = std::sqrt(dx * dx + dy * dy);
				if (len < 0.001f) continue;
				float nx = -dy / len, ny = dx / len;
				if (nx * (cx - ox) + ny * 0.f < 0.f) { nx = -nx; ny = -ny; }
				o.ax = ox;
				o.ay = cy;
				o.nx = nx;
				o.ny = ny;
			}

			f->obstacles.push_back(o);
			f->maxW = std::max(f->maxW, r.size.width);
		}
		std::sort(f->obstacles.begin(), f->obstacles.end(),
			[](Obstacle const& a, Obstacle const& b) { return a.x0 < b.x0; });
		f->built = true;
	}

	// ================= ВОЛНА =================
	void waveLogic(float dt) {
		auto f = m_fields.self();
		int legit = static_cast<int>(Mod::get()->getSettingValue<int64_t>("legit"));

		float px = m_player1->getPositionX();
		float py = m_player1->getPositionY();

		if (f->hasPrev) {
			float ddx = px - f->prevX;
			float ddy = py - f->prevY;
			if (ddx > 0.1f && std::abs(ddy) > 0.3f * ddx) {
				f->slope = std::clamp(0.7f * f->slope + 0.3f * std::abs(ddy) / ddx, 0.5f, 3.f);
				f->upSign = ((ddy > 0.f) == f->waveHold) ? 1.f : -1.f;
			}
		}

		constexpr int N = 28;
		constexpr int KMAX = N + 6;
		constexpr int OFF = KMAX;
		constexpr int W = 2 * KMAX + 1;
		constexpr int DEAD = -1000;

		float speed = speedUnits(m_player1->m_playerSpeed);
		float H = speed * 0.55f;
		float dx = H / N;
		float step = f->slope * dx;
		float r = std::clamp(5.f * std::abs(m_player1->getScale()), 3.f, 6.f);
		float yRange = (N + 6) * step + 40.f;

		f->window.clear();
		auto it = std::lower_bound(f->obstacles.begin(), f->obstacles.end(),
			px - f->maxW - 10.f,
			[](Obstacle const& o, float v) { return o.x0 < v; });
		for (; it != f->obstacles.end() && it->x0 <= px + H + 20.f; ++it) {
			if (it->x1 < px - 10.f) continue;
			if (it->y1 < py - yRange || it->y0 > py + yRange) continue;
			f->window.push_back(*it);
		}

		struct Hit { int kind = 0; Obstacle const* o = nullptr; };
		// kind: 0 свободно, 1 смерть (шип), 2 блок, 3 наклон
		auto hit = [&](float x, float y, float rr) {
			Hit h;
			for (auto const& o : f->window) {
				if (!overlapsObs(o, x, y, rr)) continue;
				if (o.kind == K_HAZARD || o.kind == K_HSLOPE) { h.kind = 1; return h; }
				if (h.kind == 0) { h.kind = (o.kind == K_SLOPE) ? 3 : 2; h.o = &o; }
			}
			return h;
		};

		auto move = [&](int i, int k, bool up, float rr) -> int {
			float x = px + (i + 1) * dx;
			int nk = k + (up ? 1 : -1);
			if (nk < -KMAX || nk > KMAX) return DEAD;
			Hit h = hit(x, py + nk * step, rr);
			if (h.kind == 0) return nk;
			if (h.kind == 1) return DEAD;
			if (h.kind == 2) {
				// блок: скользим по нему, если уже были над/под ним
				return hit(x, py + k * step, rr).kind == 0 ? k : DEAD;
			}
			// наклон: скользим вдоль него, если подошли со свободной стороны
			Obstacle const& o = *h.o;
			float fPrev = o.nx * ((x - dx) - o.ax) + o.ny * ((py + k * step) - o.ay);
			if (fPrev > 0.f) return DEAD; // врезались в торец
			int dir = (o.ny < 0.f) ? 1 : -1; // твёрдое снизу -> уходим вверх
			for (int s = 0; s <= 5; ++s) {
				int ck = nk + dir * s;
				if (ck < -KMAX || ck > KMAX) break;
				if (hit(x, py + ck * step, rr).kind == 0) return ck;
			}
			return DEAD;
		};

		auto plan = [&](float rr, int& dUp, int& dDown) {
			std::array<std::array<int, W>, N + 1> d{};
			for (int i = N - 1; i >= 1; --i) {
				int lim = std::min(i + 6, KMAX);
				for (int k = -lim; k <= lim; ++k) {
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
	// Моделируем прыжок куба по физике и ищем момент, когда прыжок
	// проходит все шипы и блоки. Прыгаем в середине безопасного окна.
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
		if (!m_player1->m_isOnGround) {
			f->w0 = -1.f;
			return;
		}

		int legit = static_cast<int>(Mod::get()->getSettingValue<int64_t>("legit"));

		float px = m_player1->getPositionX();
		float py = m_player1->getPositionY();
		float s = m_player1->m_isUpsideDown ? -1.f : 1.f;
		float pyM = s * py;
		float vx = speedUnits(m_player1->m_playerSpeed) / 60.f; // юнитов за "тик" (1/60 c)

		constexpr float V0 = 11.180032f;
		constexpr float G = 0.958199f;
		constexpr float HS = 14.5f; // блоки
		constexpr float HH = 10.f;  // шипы
		constexpr int WMAX = 48;

		// препятствия впереди (в зеркальных координатах, если гравитация перевёрнута)
		f->window.clear();
		float reach = vx * 75.f + 60.f;
		auto it = std::lower_bound(f->obstacles.begin(), f->obstacles.end(),
			px - f->maxW - 20.f,
			[](Obstacle const& o, float v) { return o.x0 < v; });
		for (; it != f->obstacles.end() && it->x0 <= px + reach; ++it) {
			if (it->x1 < px - 20.f) continue;
			if (it->kind == K_SLOPE || it->kind == K_HSLOPE) continue;
			Obstacle m = *it;
			float a = s * it->y0, b = s * it->y1;
			m.y0 = std::min(a, b);
			m.y1 = std::max(a, b);
			if (m.y1 < pyM - 120.f || m.y0 > pyM + 150.f) continue;
			f->window.push_back(m);
		}
		if (f->window.empty()) {
			f->w0 = -1.f;
			return;
		}

		auto survives = [&](float jumpAt) {
			float x = px, y = pyM, vy = 0.f;
			bool air = false;
			const float h = 0.5f;
			for (float t = 0.f; t < 72.f; t += h) {
				if (!air && jumpAt >= 0.f && t >= jumpAt) {
					air = true;
					vy = V0;
				}
				float prevY = y;
				if (air) {
					vy -= G * h;
					y += vy * h;
					if (y <= pyM && vy <= 0.f) {
						y = pyM;
						vy = 0.f;
						air = false;
					}
				}
				x += vx * h;
				for (auto const& o : f->window) {
					bool haz = (o.kind == K_HAZARD);
					float rr = haz ? HH : HS;
					if (!(x > o.x0 - rr && x < o.x1 + rr && y > o.y0 - rr && y < o.y1 + rr)) continue;
					if (haz) return false;
					if (vy <= 0.f && prevY - HS >= o.y1 - 8.f) {        // приземлились на блок
						y = o.y1 + HS;
						vy = 0.f;
						air = false;
					} else if (vy > 0.f && prevY + HS <= o.y0 + 8.f) {  // стукнулись головой
						y = o.y0 - HS;
						vy = 0.f;
					} else {
						return false;                                   // врезались боком
					}
				}
			}
			return true;
		};

		auto doJump = [&]() {
			this->handleButton(true, 1, true);
			f->holding = true;
			f->releaseTimer = 0.06f;
			f->w0 = -1.f;
			f->willMiss = false;
		};

		// если ехать прямо безопасно - ничего не делаем
		if (survives(-1.f)) {
			f->w0 = -1.f;
			return;
		}

		// безопасное окно: сколько ещё тиков можно подождать до прыжка
		int b = -1;
		if (survives(0.f)) {
			b = 0;
			while (b < WMAX && survives(static_cast<float>(b + 1))) ++b;
		}

		if (b < 0) {
			// сейчас прыгать рано/поздно. Есть ли вообще безопасный момент позже?
			bool any = false;
			for (int w = 1; w <= WMAX && !any; ++w) any = survives(static_cast<float>(w));
			f->w0 = -1.f;
			if (!any) {
				// аварийный вариант: прыгаем перед ближайшим препятствием на уровне куба
				for (auto const& o : f->window) {
					float d = o.x0 - px;
					if (d > 0.f && d <= vx * 10.f && o.y1 > pyM - 10.f && o.y0 < pyM + 10.f) {
						doJump();
						return;
					}
				}
			}
			return;
		}

		if (f->w0 < 0.f) {
			f->w0 = static_cast<float>(b);
			std::uniform_real_distribution<float> chance(0.f, 1.f);
			f->willMiss = chance(f->rng) < legit * 0.02f;
			if (legit > 0) {
				std::normal_distribution<float> noise(0.f, legit * 0.5f);
				f->jitter = noise(f->rng);
			} else {
				f->jitter = 0.f;
			}
		}
		if (f->willMiss) return;

		float threshold = f->w0 * 0.5f + f->jitter;
		if (static_cast<float>(b) <= threshold || b == 0) doJump();
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
