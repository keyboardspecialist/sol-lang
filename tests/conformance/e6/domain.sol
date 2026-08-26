module conformance.domain

public record Pair {
    left: Int64,
    right: Int64,
}

public record Packet<T> {
    ignored: Bool,
    data: (T, Bool),
}

public enum Envelope<T> {
    wrapped(packet: Packet<T>),
    empty,
}

public enum Failure {
    invalid,
}

public type Positive = refined Int64 where self > 0

public trait Score {
    function score(self: Self) -> Int64 effects { pure }
}

implementation Score for Int64 {
    function score(self: Self) -> Int64 effects { pure } {
        return self
    }
}

public function score<T: Score>(value: T) -> Int64 effects { pure } {
    return value.score()
}

public function identity<T>(value: T) -> T effects { pure } {
    return value
}
