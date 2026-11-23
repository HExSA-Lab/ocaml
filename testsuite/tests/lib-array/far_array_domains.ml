(* TEST *)

(* With several domains, minor collections share the work on all domains'
   remembered sets. Young values stored into far arrays must still be
   promoted into far memory and stay correct.

   The elements are single blocks ([ref n]): there is no allocation or poll
   point between allocating one and storing it, so it is always young when
   stored. (A multi-block value such as a list could be partly promoted
   into near memory by a minor GC triggered by another domain while it is
   being built; such old values only move to far memory at compaction.) *)

external is_far_value : 'a -> bool = "caml_is_far_value"

let work d () =
  let a = Array.make ~far:true 2000 (ref 0) in
  for round = 1 to 20 do
    for i = 0 to Array.length a - 1 do
      a.(i) <- ref (d + round + i)
    done;
    Gc.minor ();
    Array.iteri (fun i r ->
        assert (!r = d + round + i);
        assert (is_far_value r))
      a
  done;
  Gc.full_major ();
  Array.iteri (fun i r -> assert (!r = d + 20 + i)) a

let () =
  let domains = List.init 3 (fun d -> Domain.spawn (work (d * 10_000))) in
  work 50_000 ();
  List.iter Domain.join domains
