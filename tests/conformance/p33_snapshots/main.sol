module conformance.p33_snapshots

enum SnapshotFailure { rejected }

function two_snapshots(first: Int64, second: Int64, fail: Bool)
    -> Result<Int64, SnapshotFailure>
effects { pure }
requires { first > 0 }
ensures {
    success => result >= old(first) + old(second)
    failure => true
}
{
    if fail {
        return err(SnapshotFailure.rejected)
    } else {
        return ok(first + second)
    }
}

function plan_00(value: Int64) -> Int64 effects { pure }
ensures { result >= old(value) }
{ return value }

function plan_01(value: Int64) -> Int64 effects { pure }
ensures { result >= old(value) }
{ return value }

function plan_02(value: Int64) -> Int64 effects { pure }
ensures { result >= old(value) }
{ return value }

function plan_03(value: Int64) -> Int64 effects { pure }
ensures { result >= old(value) }
{ return value }

function plan_04(value: Int64) -> Int64 effects { pure }
ensures { result >= old(value) }
{ return value }

function plan_05(value: Int64) -> Int64 effects { pure }
ensures { result >= old(value) }
{ return value }

function plan_06(value: Int64) -> Int64 effects { pure }
ensures { result >= old(value) }
{ return value }

function plan_07(value: Int64) -> Int64 effects { pure }
ensures { result >= old(value) }
{ return value }

function plan_08(value: Int64) -> Int64 effects { pure }
ensures { result >= old(value) }
{ return value }

function plan_09(value: Int64) -> Int64 effects { pure }
ensures { result >= old(value) }
{ return value }

function plan_10(value: Int64) -> Int64 effects { pure }
ensures { result >= old(value) }
{ return value }

function plan_11(value: Int64) -> Int64 effects { pure }
ensures { result >= old(value) }
{ return value }

function plan_12(value: Int64) -> Int64 effects { pure }
ensures { result >= old(value) }
{ return value }

function plan_13(value: Int64) -> Int64 effects { pure }
ensures { result >= old(value) }
{ return value }

function plan_14(value: Int64) -> Int64 effects { pure }
ensures { result >= old(value) }
{ return value }

function plan_15(value: Int64) -> Int64 effects { pure }
ensures { result >= old(value) }
{ return value }

function plan_16(value: Int64) -> Int64 effects { pure }
ensures { result >= old(value) }
{ return value }

function plan_17(value: Int64) -> Int64 effects { pure }
ensures { result >= old(value) }
{ return value }

function plan_18(value: Int64) -> Int64 effects { pure }
ensures { result >= old(value) }
{ return value }

function plan_19(value: Int64) -> Int64 effects { pure }
ensures { result >= old(value) }
{ return value }
