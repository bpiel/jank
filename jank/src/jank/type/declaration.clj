(ns jank.type.declaration
  (:use clojure.walk
        clojure.pprint))

(defn shorten-types [item]
  "Walks through the decl and replaces all [:type ...] instances with
   their shorter type names. Example: [:type [:identifier \"string\"]]
   becomes (\"string\")"
  (postwalk
    (fn [x]
      (if (and (vector? x) (= :type (first x)))
        (rest (second x))
        x))
    item))

(defn function? [decl-type]
  "Returns whether or not the provided type is that of a function."
  (= "ƒ" (first decl-type)))

(defn lookup-binding [decl-name scope]
  "Recursively looks through the hierarchy of scopes for the declaration."
  (loop [current-scope scope]
    (when current-scope
      (if-let [found (find (:binding-declarations current-scope) decl-name)]
        found
        (recur (:parent current-scope))))))

(defn validate-binding [decl-name decl-type scope]
  "Looks up a declaration, if any, and verifies that the provided
   declaration has a matching type. Returns the decl or nil, if none is found."
  (let [decl (lookup-binding decl-name scope)]
    (when (some? decl)
      (let [expected-types (second decl)]
        ; All binding declarations must be the same type unless they're for
        ; function overloads. In that case, all declarations must be functions.
        (assert (or (not= -1 (.indexOf expected-types decl-type))
                    (and (function? decl-type)
                         (every? (comp function? :type) expected-types)))
                (str "declaration of "
                     decl-name
                     " as: "
                     decl-type
                     " doesn't match previous declarations: "
                     expected-types))))
    decl))

(defmulti lookup-type
  "Recursively looks through the hierarchy of scopes for the declaration."
  (fn [decl-type scope]
    (let [name (first decl-type)]
    (if (or (= "ƒ" name) (= "function" name))
      :function
      :default))))

(defmethod lookup-type :function [decl-type scope]
  (let [generics (second decl-type)]
  (pprint decl-type)
  (pprint generics)
    (assert (= (count generics) 3) "invalid function type format")
    (when (> (count (second generics)) 1)
      (assert (some? (lookup-type (second (second generics)) scope))
              "invalid function parameter type"))
    (when (> (count (nth generics 2)) 1)
      (assert (some? (lookup-type (second (nth generics 2)) scope))
              "invalid function return type"))
    decl-type))

(defmethod lookup-type :default [decl-type scope]
  "Recursively looks up a type by name.
   Returns the type, if found, or nil."
  (loop [current-scope scope]
    ; TODO: Handle generic types properly
    (when current-scope
      (if-let [found ((:type-declarations current-scope) decl-type)]
        found
        (recur (:parent current-scope))))))

(defmulti add-to-scope
  (fn [item scope]
    (let [kind (first (second item))]
      (cond
        (= :type kind)
        :type-declaration
        (= :identifier kind)
        :binding-declaration
        :else
        (assert false (str "invalid binding: " item))))))

(defmethod add-to-scope :type-declaration [item scope]
  "Adds the opaque type declaration to the scope.
   Returns the updated scope."
  (let [decl-name (first (shorten-types (rest item)))]
    (update scope :type-declarations conj decl-name)))

(defmethod add-to-scope :binding-declaration [item scope]
  "Finds, validates, and adds the provided declaration into the scope.
   Returns the updated scope."
  (let [shortened (shorten-types item)
        decl-name (get-in shortened [1 1])
        decl-type (get-in shortened [2])
        found-decl (validate-binding decl-name decl-type scope)
        found-type (lookup-type decl-type scope)]
    (assert (some? found-type) (str "unknown type: " decl-type))
    (cond
      (nil? found-decl)
      (update scope :binding-declarations assoc decl-name [{:type decl-type}])
      (and (= -1 (.indexOf (second found-decl) {:type decl-type}))
           (function? decl-type))
      (update-in scope [:binding-declarations decl-name] conj {:type decl-type})
      :else
      scope)))
