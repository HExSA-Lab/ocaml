(* * * * * * * * * * *
 * Resource Aware ML *
 * * * * * * * * * * *
 *
 * * *  Use Cases  * *
 *
 * File:
 *   examples/quicksort.raml
 *
 * Author:
 *   Jan Hoffmann, Shu-Chun Weng (2014)
 * 
 * Description:
 *   Tony Hoare's quicksort for lists.
 *   
 *)



let rec append l1 l2 =
  match l1 with
    | [] -> l2
    | x::xs -> x::(append xs l2)


let rec partition (pivot: int) (l: int list) =
  match l with
    | [] -> ([],[])
    | x::xs ->
      let (cs,bs) = partition pivot xs in
      if x > pivot then
	(cs,x::bs)
      else
	(x::cs,bs)
	

let rec quicksort = function
  | [] -> []
  | x::xs ->
      let ys, zs = partition x xs in
      append (quicksort ys) (x :: (quicksort zs))

(*
let rec from n =
    if n <= 0 then []
    else n::(from (n - 1))
*)

let _ = 
    quicksort [1;4;5;2;1;56;31;22;78] (* (from 10000) (* [4;3;2;1] (* [9;8;7;6;5;4;3;2;1] *) *) *)
