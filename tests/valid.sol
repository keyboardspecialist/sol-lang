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
