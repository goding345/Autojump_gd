#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <algorithm>
#include <random>
#include <vector>

using namespace geode::prelude;

class $modify(BotLayer, GJBaseGameLayer) {
	struct Hazard { float x, y; };

	struct Fields {
		std::vector<Hazard> hazards;   // отсортированы по X
		bool built = false;
		float lastHandledX = -1.f;     // последний обработанный шип
		float releaseTimer = 0.f;
		bool holding = false;
		std::mt19937 rng{std::random_device{}()};
	};

	void buildHazards() {
		auto f = m_fields.self();
		f->hazards.clear();
		for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
			if (obj->m_objectType == GameObjectType::Hazard) {
				f->hazards.push_back({obj->getPositionX(), obj->getPositionY()});
			}
		}
		std::sort(f->hazards.begin(), f->hazards.end(),
			[](Hazard const& a, Hazard const& b) { return a.x < b.x; });
		f->built = true;
	}

	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

		// работаем только внутри самого уровня
		if (static_cast<GJBaseGameLayer*>(PlayLayer::get()) != this) return;
		if (!Mod::get()->getSettingValue<bool>("enabled")) return;
		if (!m_player1 || m_player1->m_isDead) return;

		auto f = m_fields.self();
		if (!f->built) this->buildHazards();

		// отпускаем кнопку через короткое время после нажатия
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

		// после рестарта (practice / смерть) сбрасываем память
		if (px < f->lastHandledX) f->lastHandledX = -1.f;

		if (!m_player1->m_isOnGround) return;

		int legit = Mod::get()->getSettingValue<int64_t>("legit");

		// скорость в юнитах/сек (1x = ~311.6)
		float speed = 311.58f * m_player1->m_playerSpeed;
		float baseLead = speed * 0.17f; // за сколько юнитов до шипа прыгать

		// ищем ближайший шип впереди
		auto it = std::lower_bound(f->hazards.begin(), f->hazards.end(), px,
			[](Hazard const& h, float x) { return h.x < x; });

		for (; it != f->hazards.end(); ++it) {
			float dx = it->x - px;
			if (dx > baseLead + 60.f) break;
			if (std::abs(it->y - py) > 40.f) continue;
			if (it->x <= f->lastHandledX) continue;

			// шум по времени: чем выше legit, тем больше разброс
			std::normal_distribution<float> noise(0.f, legit * 1.6f);
			float lead = baseLead + noise(f->rng);

			if (dx <= lead) {
				f->lastHandledX = it->x;

				// шанс ошибки: 0% на 0, ~35% на 10
				std::uniform_real_distribution<float> chance(0.f, 1.f);
				bool miss = chance(f->rng) < legit * 0.035f;
				if (miss) break; // просто не прыгаем -> умрёт

				this->handleButton(true, 1, true);
				f->holding = true;
				f->releaseTimer = 0.06f;
				break;
			}
			break;
		}
	}
};
