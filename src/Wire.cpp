#include "Wire.hpp"

#include <jansson.h>

namespace hud {
namespace {
    SPanelContent parseContent(json_t* co) {
        SPanelContent content;
        if (!json_is_object(co))
            return content;
        if (json_t* k = json_object_get(co, "kind"); json_is_string(k))
            content.kind = panelKindFromName(json_string_value(k));
        if (json_t* cf = json_object_get(co, "confidence"); json_is_number(cf))
            content.confidence = static_cast<float>(json_number_value(cf));
        if (json_t* arr = json_object_get(co, "lines"); json_is_array(arr)) {
            size_t  i;
            json_t* o;
            json_array_foreach(arr, i, o) {
                SLine ln;
                if (json_t* t = json_object_get(o, "t"); json_is_string(t))
                    ln.text = json_string_value(t);
                if (json_t* c = json_object_get(o, "c"); json_is_string(c))
                    ln.color = colorFromName(json_string_value(c));
                if (json_t* b = json_object_get(o, "big"); json_is_boolean(b))
                    ln.big = json_boolean_value(b);
                content.lines.push_back(std::move(ln));
            }
        }
        if (json_t* arr = json_object_get(co, "gauges"); json_is_array(arr)) {
            size_t  i;
            json_t* o;
            json_array_foreach(arr, i, o) {
                SGauge gg;
                if (json_t* l = json_object_get(o, "label"); json_is_string(l))
                    gg.label = json_string_value(l);
                if (json_t* p = json_object_get(o, "percent"); json_is_number(p))
                    gg.percent = static_cast<float>(json_number_value(p));
                if (json_t* ch = json_object_get(o, "charging"); json_is_boolean(ch))
                    gg.charging = json_boolean_value(ch);
                content.gauges.push_back(std::move(gg));
            }
        }
        return content;
    }

    json_t* dumpContent(const SPanelContent& content) {
        json_t* co = json_object();
        json_object_set_new(co, "kind", json_string(panelKindName(content.kind)));
        if (content.confidence >= 0.f)
            json_object_set_new(co, "confidence", json_real(content.confidence));
        if (!content.lines.empty()) {
            json_t* lines = json_array();
            for (const auto& ln : content.lines) {
                json_t* o = json_object();
                json_object_set_new(o, "t", json_string(ln.text.c_str()));
                json_object_set_new(o, "c", json_string(colorName(ln.color)));
                json_object_set_new(o, "big", json_boolean(ln.big));
                json_array_append_new(lines, o);
            }
            json_object_set_new(co, "lines", lines);
        }
        if (!content.gauges.empty()) {
            json_t* gauges = json_array();
            for (const auto& g : content.gauges) {
                json_t* o = json_object();
                json_object_set_new(o, "label", json_string(g.label.c_str()));
                json_object_set_new(o, "percent", json_real(g.percent));
                json_object_set_new(o, "charging", json_boolean(g.charging));
                json_array_append_new(gauges, o);
            }
            json_object_set_new(co, "gauges", gauges);
        }
        return co;
    }
}

namespace Wire {
    bool parse(const std::string& line, SWireMsg& out) {
        json_error_t err;
        json_t*      root = json_loads(line.c_str(), 0, &err);
        if (!root || !json_is_object(root)) {
            if (root) json_decref(root);
            return false;
        }

        SWireMsg msg;
        std::string action = "upsert";
        if (json_t* a = json_object_get(root, "action"); json_is_string(a))
            action = json_string_value(a);

        auto num = [&](const char* k, double def) -> double {
            json_t* n = json_object_get(root, k);
            return json_is_number(n) ? json_number_value(n) : def;
        };
        auto u32 = [&](const char* k) -> uint32_t {
            json_t* n = json_object_get(root, k);
            return json_is_integer(n) ? static_cast<uint32_t>(json_integer_value(n)) : 0u;
        };

        if (action == "dismiss") {
            msg.action    = SWireMsg::EAction::Dismiss;
            msg.dismissId = u32("id");
            json_decref(root);
            out = std::move(msg);
            return true;
        }

        msg.action = SWireMsg::EAction::Upsert;
        SUpsert& u = msg.upsert;
        u.id = u32("id");
        if (json_t* o = json_object_get(root, "owner"); json_is_string(o))
            u.owner = json_string_value(o);
        if (json_t* s = json_object_get(root, "slot"); json_is_string(s))
            u.slot = json_string_value(s);
        if (json_t* s = json_object_get(root, "space"); json_is_string(s))
            u.space = json_string_value(s);
        u.urgency = static_cast<int>(num("urgency", 1));

        if (json_t* p = json_object_get(root, "pose"); json_is_array(p) && json_array_size(p) == 3) {
            u.hasPose = true;
            u.px = static_cast<float>(json_number_value(json_array_get(p, 0)));
            u.py = static_cast<float>(json_number_value(json_array_get(p, 1)));
            u.pz = static_cast<float>(json_number_value(json_array_get(p, 2)));
        }
        if (json_t* s = json_object_get(root, "size"); json_is_number(s)) {
            u.hasSize = true;
            u.sizeW   = static_cast<float>(json_number_value(s));
        }

        u.fade.riseMs      = static_cast<int>(num("rise", u.fade.riseMs));
        u.fade.holdMs      = static_cast<int>(num("hold", u.fade.holdMs));
        u.fade.fadeMs      = static_cast<int>(num("fade", u.fade.fadeMs));
        u.fade.opacityCeil = static_cast<float>(num("opacity", u.fade.opacityCeil));

        if (json_t* co = json_object_get(root, "content"))
            u.content = parseContent(co);

        json_decref(root);
        out = std::move(msg);
        return true;
    }

    std::string serialize(const SWireMsg& m) {
        json_t* root = json_object();
        if (m.action == SWireMsg::EAction::Dismiss) {
            json_object_set_new(root, "action", json_string("dismiss"));
            json_object_set_new(root, "id", json_integer(m.dismissId));
        } else {
            const SUpsert& u = m.upsert;
            json_object_set_new(root, "action", json_string("upsert"));
            json_object_set_new(root, "id", json_integer(u.id));
            json_object_set_new(root, "owner", json_string(u.owner.c_str()));
            json_object_set_new(root, "slot", json_string(u.slot.c_str()));
            if (!u.space.empty())
                json_object_set_new(root, "space", json_string(u.space.c_str()));
            json_object_set_new(root, "urgency", json_integer(u.urgency));
            if (u.hasPose) {
                json_t* p = json_array();
                json_array_append_new(p, json_real(u.px));
                json_array_append_new(p, json_real(u.py));
                json_array_append_new(p, json_real(u.pz));
                json_object_set_new(root, "pose", p);
            }
            if (u.hasSize)
                json_object_set_new(root, "size", json_real(u.sizeW));
            json_object_set_new(root, "rise", json_integer(u.fade.riseMs));
            json_object_set_new(root, "hold", json_integer(u.fade.holdMs));
            json_object_set_new(root, "fade", json_integer(u.fade.fadeMs));
            json_object_set_new(root, "opacity", json_real(u.fade.opacityCeil));
            json_object_set_new(root, "content", dumpContent(u.content));
        }
        char*       s   = json_dumps(root, JSON_COMPACT);
        std::string out = s ? s : "{}";
        free(s);
        json_decref(root);
        out += '\n';
        return out;
    }
}

} // namespace hud
