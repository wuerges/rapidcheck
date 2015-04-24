#pragma once

#include "rapidcheck/gen/Arbitrary.h"
#include "rapidcheck/gen/Tuple.h"
#include "rapidcheck/gen/detail/ShrinkValueIterator.h"
#include "rapidcheck/shrink/Shrink.h"
#include "rapidcheck/shrinkable/Create.h"

namespace rc {
namespace gen {
namespace detail {

RC_SFINAE_TRAIT(IsAssociativeContainer, typename T::key_type)
RC_SFINAE_TRAIT(IsMapContainer, typename T::mapped_type)

template <typename T>
using Shrinkables = std::vector<Shrinkable<T>>;

template <typename K, typename V>
using ShrinkablePairs = std::vector<Shrinkable<std::pair<K, V>>>;

template <typename Container, typename T>
Container toContainer(const Shrinkables<T> &shrinkables) {
  return Container(makeShrinkValueIterator(begin(shrinkables)),
                   makeShrinkValueIterator(end(shrinkables)));
}

template <typename T, typename Predicate>
Shrinkables<T> generateShrinkables(const Random &random,
                                   int size,
                                   std::size_t count,
                                   const Gen<T> &gen,
                                   Predicate predicate) {
  Random r(random);
  Shrinkables<T> shrinkables;
  shrinkables.reserve(count);

  int currentSize = size;
  while (shrinkables.size() < count) {
    auto shrinkable = gen(r.split(), currentSize);
    if (predicate(shrinkable)) {
      shrinkables.push_back(std::move(shrinkable));
    } else {
      // TODO give up eventually
      currentSize++;
    }
  }

  return shrinkables;
}

template <typename Container>
struct CollectionHelper {
  template <typename T>
  static Shrinkables<T> generateElements(const Random &random,
                                         int size,
                                         std::size_t count,
                                         const Gen<T> &gen) {
    return generateShrinkables(random, size, count, gen, fn::constant(true));
  }

  template <typename T>
  static Seq<Shrinkables<T>> shrinkElements(const Shrinkables<T> &shrinkables) {
    return shrink::eachElement(
        shrinkables, [](const Shrinkable<T> &s) { return s.shrinks(); });
  }
};

template <typename Container,
          bool = IsAssociativeContainer<Container>::value,
          bool = IsMapContainer<Container>::value>
struct ContainerHelper : public CollectionHelper<Container> {};

template <typename Set>
struct ContainerHelper<Set, true, false> {
  template <typename T>
  static Shrinkables<T> generateElements(const Random &random,
                                         int size,
                                         std::size_t count,
                                         const Gen<T> &gen) {
    Set set;
    return generateShrinkables(random,
                               size,
                               count,
                               gen,
                               [&](const Shrinkable<T> &s) {
                                 // We want only values that can be inserted
                                 return set.insert(s.value()).second;
                               });
  }

  template <typename T>
  static Seq<Shrinkables<T>> shrinkElements(const Shrinkables<T> &shrinkables) {
    // We use a shared_ptr here both because T might not be copyable and
    // because we don't really need to copy it since we don't modify it.
    std::shared_ptr<const Set> set =
        std::make_shared<Set>(toContainer<Set>(shrinkables));
    return shrink::eachElement(
        shrinkables,
        [=](const Shrinkable<T> &s) {
          return seq::filter(s.shrinks(),
                             [=](const Shrinkable<T> &x) {
                               // Here we filter out shrinks that collide with
                               // another value in the set because that would
                               // produce an identical set.
                               return set->find(x.value()) == set->end();
                             });
        });
  }
};

template <typename Map>
struct ContainerHelper<Map, true, true> {
  template <typename K, typename V>
  static ShrinkablePairs<K, V> generateElements(const Random &random,
                                                int size,
                                                std::size_t count,
                                                const Gen<K> &keyGen,
                                                const Gen<V> &valueGen) {
    Random r(random);
    Map map;
    auto dummyValue = valueGen(Random(), 0);
    auto keyShrinkables = generateShrinkables(
        r.split(),
        size,
        count,
        keyGen,
        [&](const Shrinkable<K> &s) {
          // We want only keys that can be inserted
          return map.insert(std::make_pair(s.value(), dummyValue.value()))
              .second;
        });

    auto valueShrinkables =
        generateShrinkables(r, size, count, valueGen, fn::constant(true));

    ShrinkablePairs<K, V> shrinkablePairs;
    shrinkablePairs.reserve(count);
    for (std::size_t i = 0; i < count; i++) {
      shrinkablePairs.push_back(shrinkable::pair(
          std::move(keyShrinkables[i]), std::move(valueShrinkables[i])));
    }

    return shrinkablePairs;
  }

  template <typename K, typename V>
  static Seq<ShrinkablePairs<K, V>>
  shrinkElements(const ShrinkablePairs<K, V> &shrinkablePairs) {
    // We use a shared_ptr here both because K and V might not be copyable
    // and because we don't really need to copy it since we don't modify it.
    std::shared_ptr<const Map> map =
        std::make_shared<Map>(toContainer<Map>(shrinkablePairs));
    return shrink::eachElement(
        shrinkablePairs,
        [=](const Shrinkable<std::pair<K, V>> &elem) {
          return seq::filter(
              elem.shrinks(),
              [=](const Shrinkable<std::pair<K, V>> &elemShrink) {
                // Here we filter out values with keys that collide
                // with other keys of the map. However, if the key
                // is the same, that means that something else
                // in this shrink since we expect shrinks to not
                // equal the original.
                // NOTE: This places the restriction that the key must
                // have an equality operator that works but that's
                // usually true for types used as keys anyway.
                const auto shrinkValue = elemShrink.value();
                return (map->find(shrinkValue.first) == map->end()) ||
                    (shrinkValue.first == elem.value().first);
              });
        });
  }
};

template <typename MultiMap>
struct MultiMapHelper : public CollectionHelper<MultiMap> {
  template <typename K, typename V>
  static ShrinkablePairs<K, V> generateElements(const Random &random,
                                                int size,
                                                std::size_t count,
                                                const Gen<K> &keyGen,
                                                const Gen<V> &valueGen) {
    // We treat this as a normal collection since we don't need to worry
    // about duplicate keys et.c.
    return CollectionHelper<MultiMap>::generateElements(
        random, size, count, gen::pair(keyGen, valueGen));
  }
};

template <typename... Args>
struct ContainerHelper<std::multiset<Args...>, true, false>
    : public CollectionHelper<std::multiset<Args...>> {};

template <typename... Args>
struct ContainerHelper<std::unordered_multiset<Args...>, true, false>
    : public CollectionHelper<std::unordered_multiset<Args...>> {};

template <typename... Args>
struct ContainerHelper<std::multimap<Args...>, true, true>
    : public MultiMapHelper<std::multimap<Args...>> {};

template <typename... Args>
struct ContainerHelper<std::unordered_multimap<Args...>, true, true>
    : public MultiMapHelper<std::unordered_multimap<Args...>> {};

template <typename Container>
struct GenerateContainer {
  template <typename... Ts>
  static Shrinkable<Container>
  generate(const Random &random, int size, Gen<Ts>... gens) {
    using Helper = ContainerHelper<Container>;

    Random r(random);
    std::size_t count = r.split().next() % (size + 1);
    auto shrinkables = Helper::generateElements(r, size, count, gens...);

    using Elements = decltype(shrinkables);
    return shrinkable::map(
        shrinkable::shrinkRecur(std::move(shrinkables),
                                [](const Elements &elements) {
                                  return seq::concat(
                                      shrink::removeChunks(elements),
                                      Helper::shrinkElements(elements));
                                }),
        &toContainer<Container, typename Elements::value_type::ValueType>);
  }

  template <typename... Ts>
  static Shrinkable<Container>
  generate(std::size_t count, const Random &random, int size, Gen<Ts>... gens) {
    using Helper = ContainerHelper<Container>;

    auto shrinkables = Helper::generateElements(random, size, count, gens...);

    using Elements = decltype(shrinkables);
    return shrinkable::map(
        shrinkable::shrinkRecur(std::move(shrinkables),
                                [](const Elements &elements) {
                                  return Helper::shrinkElements(elements);
                                }),
        &toContainer<Container, typename Elements::value_type::ValueType>);
  }
};

template <typename T, std::size_t N>
struct GenerateContainer<std::array<T, N>> {
  using Array = std::array<T, N>;

  template <typename U>
  static Shrinkable<Array>
  generate(const Random &random, int size, const Gen<U> &gen) {
    return shrinkable::map(
        shrinkable::shrinkRecur(
            generateShrinkables(random, size, N, gen, fn::constant(true)),
            [](const Shrinkables<U> &elements) {
              return CollectionHelper<Array>::shrinkElements(elements);
            }),
        [](const Shrinkables<U> &elements) {
          Array array;
          for (std::size_t i = 0; i < N; i++)
            array[i] = elements[i].value();
          return array;
        });
  }

  template <typename U>
  static Shrinkable<Array> generate(std::size_t count,
                                    const Random &random,
                                    int size,
                                    const Gen<U> &gen) {
    if (count != N) {
      throw GenerationFailure(
          "Count must be equal to length of array for std::array");
    }
    return generate(random, size, gen);
  }
};

template <typename Container>
struct ContainerArbitrary1 {
  static Gen<Container> arbitrary() {
    return gen::container<Container>(
        gen::arbitrary<typename Container::value_type>());
  }
};

template <typename Container>
struct ContainerArbitrary2 {
  static Gen<Container> arbitrary() {
    return gen::container<Container>(
        gen::arbitrary<typename Container::key_type>(),
        gen::arbitrary<typename Container::mapped_type>());
  }
};

#define SPECIALIZE_SEQUENCE_ARBITRARY1(Container)                              \
  template <typename... Args>                                                  \
  class DefaultArbitrary<Container<Args...>>                                   \
      : public gen::detail::ContainerArbitrary1<Container<Args...>> {};

#define SPECIALIZE_SEQUENCE_ARBITRARY2(Container)                              \
  template <typename... Args>                                                  \
  class DefaultArbitrary<Container<Args...>>                                   \
      : public gen::detail::ContainerArbitrary2<Container<Args...>> {};

SPECIALIZE_SEQUENCE_ARBITRARY1(std::vector)
SPECIALIZE_SEQUENCE_ARBITRARY1(std::deque)
SPECIALIZE_SEQUENCE_ARBITRARY1(std::forward_list)
SPECIALIZE_SEQUENCE_ARBITRARY1(std::list)
SPECIALIZE_SEQUENCE_ARBITRARY1(std::set)
SPECIALIZE_SEQUENCE_ARBITRARY1(std::multiset)
SPECIALIZE_SEQUENCE_ARBITRARY1(std::unordered_set)
SPECIALIZE_SEQUENCE_ARBITRARY1(std::unordered_multiset)

SPECIALIZE_SEQUENCE_ARBITRARY2(std::map)
SPECIALIZE_SEQUENCE_ARBITRARY2(std::multimap)
SPECIALIZE_SEQUENCE_ARBITRARY2(std::unordered_map)
SPECIALIZE_SEQUENCE_ARBITRARY2(std::unordered_multimap)

#undef SPECIALIZE_SEQUENCE_ARBITRARY1
#undef SPECIALIZE_SEQUENCE_ARBITRARY2

// std::array is a bit special since it has non-type template params
template <typename T, std::size_t N>
struct DefaultArbitrary<std::array<T, N>> {
  static Gen<std::array<T, N>> arbitrary() {
    return gen::container<std::array<T, N>>(gen::arbitrary<T>());
  }
};

} // namespace detail

template <typename Container, typename... Ts>
Gen<Container> container(Gen<Ts>... gens) {
  return [=](const Random &random, int size) {
    return detail::GenerateContainer<Container>::generate(
        random, size, gens...);
  };
}

template <typename Container, typename... Ts>
Gen<Container> container(std::size_t count, Gen<Ts>... gens) {
  return [=](const Random &random, int size) {
    return detail::GenerateContainer<Container>::generate(
        count, random, size, gens...);
  };
}

} // namespace gen
} // namespace rc