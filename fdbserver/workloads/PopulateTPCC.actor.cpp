#include "flow/actorcompiler.h"
#include "workloads.actor.h"
#include "QuietDatabase.actor.h"
#include "TPCCWorkload.h"

#include <fdbclient/ReadYourWrites.h>

#undef FLOW_ACOMPILER_STATE
#define FLOW_ACOMPILER_STATE 1

using namespace TPCCWorkload;

namespace {

constexpr char alphaNumerics[] = { 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i',
                                   'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r',
                                   's', 't', 'u', 'v', 'w', 'x', 'y', 'z', 'A',
                                   'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J',
                                   'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S',
                                   'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '1', '2',
                                   '3', '4', '5', '6', '7', '8', '9', '0' };
constexpr char numerics[] = {
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9'
};

constexpr const char* originalString = "ORIGINAL";

struct PopulateTPCC : TestWorkload
{
    static constexpr const char* DESCRIPTION = "PopulateTPCC";

    int actorsPerClient;
    int warehousesPerActor;
    int clientsUsed;

    GlobalState gState;

    PopulateTPCC(WorkloadContext const& ctx)
      : TestWorkload(ctx)
    {
        std::string workloadName = DESCRIPTION;
        GETOPTION(actorsPerClient, 10);
        GETOPTION(warehousesPerActor, 30);
        GETOPTION(clientsUsed, 2);
    }

    int NURand(int C, int A, int x, int y)
    {
        return (((g_random->randomInt(0, A + 1) |
                  g_random->randomInt(x, y + 1)) +
                 C) %
                (y - x + 1)) +
               x;
    }

    StringRef aString(Arena& arena, int x, int y)
    {
        int length = g_random->randomInt(x, y + 1);
        char* res = new (arena) char[length];
        for (int i = 0; i < length; ++i) {
            res[i] =
                alphaNumerics[g_random->randomInt(0, sizeof(alphaNumerics))];
        }
        return StringRef(reinterpret_cast<uint8_t*>(res), length);
    }

    StringRef nString(Arena& arena, int x, int y)
    {
        int length = g_random->randomInt(x, y + 1);
        char* res = new (arena) char[length];
        for (int i = 0; i < length; ++i) {
            res[i] = numerics[g_random->randomInt(0, sizeof(numerics))];
        }
        return StringRef(reinterpret_cast<uint8_t*>(res), length);
    }

    StringRef genCLast(Arena& arena, int x)
    {
        int l = x % 10;
        x /= 10;
        int m = x % 10;
        x /= 10;
        int f = x % 10;
        std::stringstream ss;
        ss << syllables[f] << syllables[m] << syllables[l];
        return StringRef(arena, ss.str());
    }

    StringRef rndZip(Arena& arena)
    {
        char* result = new (arena) char[9];
        for (int i = 0; i < 4; ++i) {
            result[i] = numerics[g_random->randomInt(0, sizeof(numerics))];
        }
        for (int i = 4; i < 9; ++i) {
            result[i] = '1';
        }
        return StringRef(reinterpret_cast<uint8_t*>(result), 9);
    }

    StringRef dataString(Arena& arena)
    {
        if (g_random->random01() < 0.1) {
            auto str = aString(arena, 26, 51 - strlen(originalString));
            char* r = new (arena) char[str.size() + strlen(originalString)];
            int pos = g_random->randomInt(0, str.size());
            std::copy(originalString, originalString + strlen(originalString),
                      r + pos);
            auto res = reinterpret_cast<uint8_t*>(r);
            std::copy(str.begin(), str.begin() + pos, res);
            std::copy(str.begin() + pos, str.end(),
                      res + pos + strlen(originalString));
            return StringRef(res, str.size() + strlen(originalString));
        } else {
            return aString(arena, 26, 51);
        }
    }

    ACTOR static Future<Void> writeGlobalState(PopulateTPCC* self, Database cx)
    {
        state ReadYourWritesTransaction tr(cx);
        loop
        {
            tr.reset();
            try {
                BinaryWriter writer(IncludeVersion());
                serializer(writer, v1(self->gState));
                tr.set(self->gState.key(), writer.toStringRef());
                Void _uvar = wait(tr.commit());
                return Void();
            } catch (Error& e) {
                wait(tr.onError(e));
            }
        }
    }

    ACTOR static Future<Void> readGlobalState(PopulateTPCC* self, Database cx)
    {
        state ReadYourWritesTransaction tr(cx);
        loop
        {
            tr.reset();
            try {
                Optional<Value> val = wait(tr.get(self->gState.key()));
                if (val.present()) {
                    BinaryReader reader(val.get(), IncludeVersion());
                    serializer(reader, v1(self->gState));
                } else {
                    Void _uvar = wait(delay(1.0));
                }
                return Void();
            } catch (Error& e) {
                wait(tr.onError(e));
            }
        }
    }

    virtual std::string description() override { return DESCRIPTION; }

    ACTOR static Future<Void> populateItems(PopulateTPCC* self, Database cx)
    {
        state Transaction tr(cx);
        state int itemStart = 0;
        state int i_id;
        for (; itemStart < 100000; itemStart += 100) {
            TraceEvent("PopulateItems").detail("Status", itemStart);
            loop
            {
                try {
                    tr.reset();
                    for (i_id = itemStart; i_id < itemStart + 100; ++i_id) {
                        Item item;
                        item.i_id = i_id;
                        item.i_im_id = g_random->randomInt(1, 10001);
                        item.i_name = self->aString(item.arena, 14, 25);
                        item.i_price = g_random->randomInt64(1.0, 100.0);
                        item.i_data = self->dataString(item.arena);
                        BinaryWriter w(IncludeVersion());
                        serializer(w, v1(item));
                        tr.set(item.key(), w.toStringRef(), false);
                    }
                    Void _uvar = wait(tr.commit());
                    break;
                } catch (Error& e) {
                    TraceEvent("PopulateItemsHandleError").error(e);
                    Void _uvar = wait(tr.onError(e));
                }
            }
        }
        TraceEvent("PopulateItemsDone");
        return Void();
    }

    ACTOR static Future<Void> populateCustomers(PopulateTPCC* self, Database cx,
                                                int w_id, int d_id)
    {
        state Transaction tr(cx);
        state int cStart;
        state int c_id;
        for (cStart = 0; cStart < 3000; cStart += 100) {
            TraceEvent("PopulateCustomers")
                .detail("Warehouse", w_id)
                .detail("District", d_id)
                .detail("Customer", cStart);
            loop
            {
                for (c_id = cStart; c_id < cStart + 100; ++c_id) {
                    Customer c;
                    History h;
                    c.c_id = c_id;
                    c.c_d_id = d_id;
                    c.c_w_id = w_id;
                    if (c_id < 1000) {
                        c.c_last = self->genCLast(c.arena, c_id);
                    } else {
                        c.c_last = self->genCLast(
                            c.arena,
                            self->NURand(self->gState.CLoad, 255, 0, 999));
                    }
                    c.c_middle = LiteralStringRef("OE");
                    c.c_first = self->aString(c.arena, 8, 16);
                    c.c_street_1 = self->aString(c.arena, 10, 20);
                    c.c_street_2 = self->aString(c.arena, 10, 20);
                    c.c_city = self->aString(c.arena, 10, 20);
                    c.c_state = self->aString(c.arena, 2, 2);
                    c.c_zip = self->rndZip(c.arena);
                    c.c_phone = self->nString(c.arena, 16, 16);
                    c.c_since = g_network->now();
                    if (g_random->random01() < 0.1) {
                        c.c_credit = LiteralStringRef("BC");
                    } else {
                        c.c_credit = LiteralStringRef("GC");
                    }
                    c.c_credit_lim = 50000;
                    c.c_discount = g_random->random01() / 2.0;
                    c.c_balance = -10.0;
                    c.c_ytd_payment = 10.0;
                    c.c_payment_cnt = 1;
                    c.c_delivery_count = 0;
                    c.c_data = self->aString(c.arena, 300, 500);

                    h.h_c_id = c_id;
                    h.h_c_d_id = d_id;
                    h.h_d_id = d_id;
                    h.h_w_id = w_id;
                    h.h_c_w_id = w_id;
                    h.h_date = g_network->now();
                    h.h_amount = 10.0;
                    h.h_data = self->aString(c.arena, 12, 24);
                    {
                        BinaryWriter w(IncludeVersion());
                        serializer(w, v1(c));
                        tr.set(c.key(), w.toStringRef(), false);
                    }
                    {
                        // Write index
                        tr.set(c.indexLastKey(), c.key(), false);
                    }
                    {
                        BinaryWriter w(IncludeVersion());
                        serializer(w, v1(h));
                        UID k = g_random->randomUniqueID();
                        BinaryWriter kW(Unversioned());
                        serializer(kW, v1(k));
                        auto key = kW.toStringRef().withPrefix(
                            LiteralStringRef("History/"));
                        tr.set(key, w.toStringRef(), false);
                    }
                }
                try {
                    Void _uvar = wait(tr.commit());
                    break;
                } catch (Error& e) {
                    TraceEvent("PopulateCustomerHandleError").error(e);
                    Void _uvar = wait(tr.onError(e));
                }
            }
        }
        TraceEvent("PopulateCustomersDone")
            .detail("Warehouse", w_id)
            .detail("District", d_id);
        return Void();
    }

    ACTOR static Future<Void> populateOrders(PopulateTPCC* self, Database cx,
                                             int w_id, int d_id)
    {
        state Transaction tr(cx);
        state std::vector<int> customerIds;
        state int idStart;
        state int o_id;
        customerIds.reserve(3000);
        for (int i = 0; i < 3000; ++i) {
            customerIds.push_back(i);
        }
        g_random->randomShuffle(customerIds);
        for (idStart = 0; idStart < 3000; idStart += 100) {
            TraceEvent("PopulateOrders")
                .detail("Warehouse", w_id)
                .detail("District", d_id)
                .detail("Order", idStart);
            loop
            {
                tr.reset();
                for (o_id = idStart; o_id < idStart + 100; ++o_id) {
                    Order o;
                    o.o_id = o_id;
                    o.o_c_id = customerIds[o_id];
                    o.o_d_id = d_id;
                    o.o_w_id = w_id;
                    o.o_entry_d = g_network->now();
                    if (o_id < 2100) {
                        o.o_carrier_id = g_random->randomInt(1, 11);
                    }
                    o.o_ol_cnt = g_random->randomInt(5, 16);
                    o.o_all_local = true;
                    for (int ol_number = 0; ol_number < o.o_ol_cnt;
                         ++ol_number) {
                        OrderLine ol;
                        ol.ol_o_id = o_id;
                        ol.ol_d_id = d_id;
                        ol.ol_w_id = w_id;
                        ol.ol_number = ol_number;
                        ol.ol_i_id = g_random->randomInt(0, 100000);
                        ol.ol_supply_w_id = w_id;
                        if (o_id < 2100) {
                            ol.ol_delivery_d = g_network->now();
                            ol.ol_amount = 0.0;
                        } else {
                            ol.ol_amount = g_random->random01() * 10000.0;
                        }
                        ol.ol_quantity = 5;
                        ol.ol_dist_info = self->aString(ol.arena, 24, 24);
                        BinaryWriter w(IncludeVersion());
                        serializer(w, v1(ol));
                        tr.set(ol.key(), w.toStringRef(), false);
                    }
                    BinaryWriter w(IncludeVersion());
                    serializer(w, v1(o));
                    tr.set(o.key(), w.toStringRef(), false);
                }
                try {
                    Void _uvar = wait(tr.commit());
                    break;
                } catch (Error& e) {
                    TraceEvent("PopulateOrderHandleError").error(e);
                    Void _uvar = wait(tr.onError(e));
                }
            }
        }
        TraceEvent("PopulateOrdersDone")
            .detail("Warehouse", w_id)
            .detail("District", d_id);
        return Void();
    }

    ACTOR static Future<Void> populateNewOrders(PopulateTPCC* self, Database cx,
                                                int w_id, int d_id)
    {
        state Transaction tr(cx);
        TraceEvent("PopulateNewOrders")
            .detail("Warehouse", w_id)
            .detail("District", d_id);
        loop
        {
            tr.reset();
            for (int i = 2100; i < 3000; ++i) {
                NewOrder no;
                no.no_o_id = i;
                no.no_d_id = d_id;
                no.no_w_id = w_id;
                BinaryWriter w(IncludeVersion());
                serializer(w, v1(no));
                tr.set(no.key(), w.toStringRef(), false);
            }
            try {
                Void _uvar = wait(tr.commit());
                break;
            } catch (Error& e) {
                TraceEvent("PopulateNewOrderHandleError").error(e);
                Void _uvar = wait(tr.onError(e));
            }
        }
        TraceEvent("PopulateNewOrdersDone")
            .detail("Warehouse", w_id)
            .detail("District", d_id);
        return Void();
    }

    ACTOR static Future<Void> populateDistricts(PopulateTPCC* self, Database cx,
                                                int w_id)
    {
        state Transaction tr(cx);
        state int d_id;
        for (d_id = 0; d_id < 10; ++d_id) {
            TraceEvent("PopulateDistricts")
                .detail("Warehouse", w_id)
                .detail("District", d_id);
            loop
            {
                tr.reset();
                District d;
                d.d_id = d_id;
                d.d_w_id = w_id;
                d.d_name = self->aString(d.arena, 6, 10);
                d.d_street_1 = self->aString(d.arena, 10, 20);
                d.d_street_2 = self->aString(d.arena, 10, 20);
                d.d_city = self->aString(d.arena, 10, 20);
                d.d_state = self->aString(d.arena, 2, 2);
                d.d_zip = self->rndZip(d.arena);
                d.d_tax = g_random->random01() * 0.2;
                d.d_ytd = 30000;
                d.d_next_o_id = 3000;
                BinaryWriter w(IncludeVersion());
                serializer(w, v1(d));
                tr.set(d.key(), w.toStringRef(), false);
                try {
                    Void _uvar = wait(tr.commit());
                    Void _uvar = wait(populateCustomers(self, cx, w_id, d_id));
                    Void _uvar = wait(populateOrders(self, cx, w_id, d_id));
                    Void _uvar = wait(populateNewOrders(self, cx, w_id, d_id));
                    break;
                } catch (Error& e) {
                    TraceEvent("PopulateDistrictHandleError").error(e);
                    Void _uvar = wait(tr.onError(e));
                }
            }
        }
        TraceEvent("PopulateDistrictsDone").detail("Warehouse", w_id);
        return Void();
    }

    ACTOR static Future<Void> populateStock(PopulateTPCC* self, Database cx,
                                            int w_id)
    {
        state Transaction tr(cx);
        state int idStart;
        for (idStart = 0; idStart < 100000; idStart += 100) {
            TraceEvent("PopulateStock")
                .detail("Warehouse", w_id)
                .detail("i_id", idStart);
            loop
            {
                tr.reset();
                for (int i = idStart; i < idStart + 100; ++i) {
                    Stock s;
                    s.s_i_id = i;
                    s.s_w_id = w_id;
                    s.s_quantity = g_random->randomInt(1, 101);
                    s.s_dist_01 = self->aString(s.arena, 24, 25);
                    s.s_dist_02 = self->aString(s.arena, 24, 25);
                    s.s_dist_03 = self->aString(s.arena, 24, 25);
                    s.s_dist_04 = self->aString(s.arena, 24, 25);
                    s.s_dist_05 = self->aString(s.arena, 24, 25);
                    s.s_dist_06 = self->aString(s.arena, 24, 25);
                    s.s_dist_07 = self->aString(s.arena, 24, 25);
                    s.s_dist_08 = self->aString(s.arena, 24, 25);
                    s.s_dist_09 = self->aString(s.arena, 24, 25);
                    s.s_dist_10 = self->aString(s.arena, 24, 25);
                    s.s_ytd = 0;
                    s.s_order_cnt = 0;
                    s.s_remote_cnt = 0;
                    s.s_data = self->dataString(s.arena);
                    BinaryWriter w(IncludeVersion());
                    serializer(w, v1(s));
                    tr.set(s.key(), w.toStringRef(), false);
                }
                try {
                    Void _uvar = wait(tr.commit());
                    break;
                } catch (Error& e) {
                    TraceEvent("PopulateStockHandleError")
                        .error(e)
                        .detail("Warehouse", w_id);
                    Void _uvar = wait(tr.onError(e));
                }
            }
        }
        TraceEvent("PopulateStockDone").detail("Warehouse", w_id);
        return Void();
    }

    ACTOR static Future<Void> populateWarehouse(PopulateTPCC* self, Database cx,
                                                int w_id)
    {
        state Transaction tr(cx);
        TraceEvent("PopulateWarehouse").detail("W_ID", w_id);
        loop
        {
            tr.reset();
            try {
                Warehouse w;
                w.w_id = w_id;
                w.w_name = self->aString(w.arena, 6, 11);
                w.w_street_1 = self->aString(w.arena, 10, 21);
                w.w_street_2 = self->aString(w.arena, 10, 21);
                w.w_city = self->aString(w.arena, 10, 21);
                w.w_state = self->aString(w.arena, 2, 3);
                w.w_tax = g_random->random01() * 0.2;
                w.w_ytd = 300000;
                BinaryWriter writer(IncludeVersion());
                serializer(writer, v1(w));
                tr.set(w.key(), writer.toStringRef(), false);
                Void _uvar = wait(tr.commit());
                break;
            } catch (Error& e) {
                TraceEvent("PopulateWarehouseHandleError")
                    .error(e)
                    .detail("Warehouse", w_id);
                Void _uvar = wait(tr.onError(e));
            }
        }
        Void _uvar = wait(populateStock(self, cx, w_id));
        Void _uvar = wait(populateDistricts(self, cx, w_id));
        TraceEvent("PopulateWarehouseDone").detail("W_ID", w_id);
        return Void();
    }

    ACTOR static Future<Void> populateActor(PopulateTPCC* self, Database cx,
                                            int actorId)
    {
        state int startWID =
            self->clientId * self->actorsPerClient * self->warehousesPerActor +
            actorId * self->warehousesPerActor;
        state int endWID = startWID + self->warehousesPerActor;
        state int wid;
        for (wid = startWID; wid < endWID; ++wid) {
            Void _uvar = wait(populateWarehouse(self, cx, wid));
        }
        return Void();
    }

    ACTOR static Future<Void> populate(PopulateTPCC* self, Database cx)
    {
        if (self->clientId == 0) {
            Void _uvar = wait(writeGlobalState(self, cx));
        } else {
            Void _uvar = wait(readGlobalState(self, cx));
        }
        if (self->clientId == 0) {
            Void _uvar = wait(populateItems(self, cx));
        }

        state vector<Future<Void>> populateActors;
        state int actorId;
        for (actorId = 0; actorId < self->actorsPerClient; ++actorId) {
            populateActors.push_back(populateActor(self, cx, actorId));
        }
        Void _uvar = wait(waitForAll(populateActors));
        Void _uvar = wait(quietDatabase(cx, self->dbInfo, "PopulateTPCC"));
        return Void();
    }

    Future<Void> setup(Database const& cx) override
    {
        if (clientId >= clientsUsed)
            return Void();
        return populate(this, cx);
    }

    virtual Future<Void> start(Database const& cx) override { return Void(); }

    virtual Future<bool> check(Database const& cx) override { return true; }

    virtual void getMetrics(vector<PerfMetric>& m) override {}
};

} // namespace

WorkloadFactory<PopulateTPCC> PopulateTPCCWorkloadFactory(
    PopulateTPCC::DESCRIPTION);
