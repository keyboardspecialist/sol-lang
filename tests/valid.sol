module demo.main
edition 2027

use core.Text

@stable("demo.User.v1")
public record User {
    name: Text,
}

enum GreetError {
    invalid(field: Text),
}

record Box<T> {
    value: T,
}

enum Either<L, R> {
    left(value: L),
    right(value: R),
}

function identity<T>(value: T) -> T effects { pure } {
    return value
}

function keep_text(value: Text) -> Text effects { pure } {
    return value
}

function load_text(value: Text) -> Text effects { service.read } {
    return value
}

function invoke<T, effects E>(
    value: T,
    callback: function(T) -> T effects E,
) -> T effects { E } {
    return callback(value)
}

function invoke_pure(value: Text) -> Text effects { pure } {
    return invoke(value, keep_text)
}

function invoke_read(value: Text) -> Text effects { service.read } {
    return invoke<Text>(value, load_text)
}

trait Display {
    function display(self: Self) -> Text effects { pure }
}

implementation Display for Int64 {
    function display(self: Self) -> Text effects { pure } {
        return "number"
    }
}

function display_value<T: Display>(value: T) -> Text effects { pure } {
    return value.display()
}

function display_count(value: Int64) -> Text effects { pure } {
    return display_value(value)
}

function boxed_name(user: User) -> Box<Text> effects { pure } {
    return Box<Text> { value = identity(user.name) }
}

function left_name(user: User) -> Either<Text, GreetError> effects { pure } {
    return Either<Text, GreetError>.left(value = identity<Text>(user.name))
}

capability Clock {
    function now() -> Int64
    effects {
        clock.read<Self>
    }
}

capability TimestampClock derives_from source: capability Clock {
    function now() -> Int64
    effects {
        clock.read<Self>
    } {
        return source.now()
    }
}

capability Random {
    function next() -> Int64
    effects {
        random.read<Self>
    }
}

capability FixedRandom {
    function next() -> Int64
    effects {
        pure
    }
}

public function greet(
    user: User,
    clock: capability Clock,
) -> Result<Text, GreetError>
effects {
    clock.read<clock>
}
requires {
    user.name == user.name
}
ensures {
    success => result == old(user.name)
    failure => user.name == old(user.name)
} {
    return ok(user.name)
}

function timestamp(clock: capability Clock) -> Int64
effects {
    clock.read<clock>
} {
    return clock.now()
}

function restricted_timestamp(clock: capability Clock) -> Int64
effects {
    clock.read<clock>
} {
    let restricted = TimestampClock { source = clock }
    return restricted.now()
}

function fixed_random(
    random: capability Random,
    provider: capability FixedRandom,
) -> Int64
effects {
    pure
} {
    return handle random.read<random> with provider {
        random.next()
    }
}
