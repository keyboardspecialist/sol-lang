module conformance.p43_sums_wrappers

record Pair { left: Int64, right: Bool }
enum Problem { bad }
enum Signal { payload(value: Int64), empty }
enum Envelope { packed(value: Pair), vacant }
enum Context { option(value: Option<Int64>), result(value: Result<Int64, Problem>) }

type Count = distinct Int64
type Label = distinct Text
type Packed = distinct Pair

@entry
public function first() -> Int64 effects { pure } {
    let present = Context.option(some(7))
    let present_copy = present
    let absent = Context.option(none())
    let absent_copy = absent
    let good = Context.result(ok(9))
    let good_copy = good
    let bad = Context.result(err(Problem.bad))
    let bad_copy = bad
    let live = Signal.payload(11)
    let empty_signal = Signal.empty
    let envelope = Envelope.packed(Pair { left = 20, right = true })
    let envelope_copy = envelope
    let count = Count(3)
    let count_copy = count
    let label = Label("sum-wrapper")
    let label_copy = label
    let packed_value = Packed(Pair { left = 4, right = true })
    let packed_copy = packed_value
    return if present == present_copy && absent == absent_copy && good == good_copy
        && bad == bad_copy && live == live && empty_signal == empty_signal
        && envelope == envelope_copy && count == count_copy && label == label_copy
        && packed_value == packed_copy { 42 } else { 0 }
}

function second() -> Int64 effects { pure } { return 0 }
